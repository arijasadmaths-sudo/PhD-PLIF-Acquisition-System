/* PC camera receiver. Build with the Teledyne GigE-V SDK example makefile. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "cordef.h"
#include "gevapi.h"
#include "SapX11Util.h"
#include "FileUtil.h"

#ifndef LIBTIFF_AVAILABLE
#error "TIFF support is required. Install libtiff development files and rebuild the SDK example."
#endif

#define BUFFER_COUNT 8
#define COMMAND_SIZE 64
#define FEATURE_COUNT 24
#define FRAME_MAX_AGE 2.0
#define FIRST_FRAME_TIMEOUT 5.0
#define MAX_IMAGE_BYTES ((size_t)512 * 1024 * 1024)

static volatile sig_atomic_t interrupted = 0;

struct settings {
    const char *camera_ip;
    const char *bind_ip;
    const char *output;
    unsigned short port;
    int once;
    int feature_count;
    const char *features[FEATURE_COUNT];
};

struct camera {
    GEV_CAMERA_HANDLE handle;
    UINT8 *buffers[BUFFER_COUNT];
    UINT32 width, height, format;
    size_t image_bytes, buffer_bytes;
    unsigned char *latest;
    unsigned long long sequence;
    double received_at;
    int stop, failed, api_ready, transfer_ready, thread_ready;
    pthread_t thread;
    pthread_mutex_t mutex;
    char error[192];
};

static double monotonic_seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0.0;
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void on_signal(int signum)
{
    (void)signum;
    interrupted = 1;
}

static int sdk_ok(GEV_STATUS status, const char *operation)
{
    if (status == GEVLIB_OK) return 1;
    fprintf(stderr, "%s failed (GigE-V status %d).\n", operation, (int)status);
    return 0;
}

static void capture_error(struct camera *camera, const char *message)
{
    pthread_mutex_lock(&camera->mutex);
    camera->failed = 1;
    snprintf(camera->error, sizeof(camera->error), "%s", message);
    pthread_mutex_unlock(&camera->mutex);
}

static void *receive_frames(void *argument)
{
    struct camera *camera = argument;
    for (;;) {
        GEV_BUFFER_OBJECT *image = NULL;
        GEV_STATUS status;
        int stop;
        pthread_mutex_lock(&camera->mutex);
        stop = camera->stop;
        pthread_mutex_unlock(&camera->mutex);
        if (stop) break;
        status = GevWaitForNextImage(camera->handle, &image, 500);
        if (image != NULL && status == GEVLIB_OK && image->status == 0) {
            if (image->address == NULL || image->w != camera->width ||
                image->h != camera->height ||
                GevGetConvertedPixelType(0, image->format) != camera->format) {
                capture_error(camera, "Received image geometry or pixel format changed; restart the receiver.");
            } else {
                /* The SDK owns this buffer until release. Keep a private copy. */
                pthread_mutex_lock(&camera->mutex);
                memcpy(camera->latest, image->address, camera->image_bytes);
                camera->received_at = monotonic_seconds();
                camera->sequence++;
                pthread_mutex_unlock(&camera->mutex);
            }
        }
        /* Release incomplete frames too, so they cannot exhaust the buffer pool. */
        if (image != NULL && GevReleaseImage(camera->handle, image) != GEVLIB_OK)
            capture_error(camera, "Could not release an SDK image buffer.");
        pthread_mutex_lock(&camera->mutex);
        stop = camera->failed;
        pthread_mutex_unlock(&camera->mutex);
        if (stop) break;
    }
    return NULL;
}

static void close_camera(struct camera *camera)
{
    pthread_mutex_lock(&camera->mutex);
    camera->stop = 1;
    pthread_mutex_unlock(&camera->mutex);
    if (camera->thread_ready) pthread_join(camera->thread, NULL);
    if (camera->transfer_ready) {
        GevAbortTransfer(camera->handle);
        GevFreeTransfer(camera->handle);
    }
    if (camera->handle != NULL) GevCloseCamera(&camera->handle);
    for (int i = 0; i < BUFFER_COUNT; i++) free(camera->buffers[i]);
    free(camera->latest);
    if (camera->api_ready) {
        GevApiUninitialize();
        _CloseSocketAPI();
    }
    pthread_mutex_destroy(&camera->mutex);
}

static void describe_feature(GEV_CAMERA_HANDLE handle, FILE *file, const char *name)
{
    char value[512] = {0};
    int type = 0;
    if (GevGetFeatureValueAsString(handle, name, &type, sizeof(value), value) == GEVLIB_OK)
        fprintf(file, "%s=%s\n", name, value);
    else
        fprintf(file, "%s=unavailable\n", name);
}

static int open_camera(struct camera *camera, const struct settings *settings, FILE *metadata)
{
    struct in_addr address;
    GEV_DEVICE_INTERFACE devices[32];
    GEV_CAMERA_OPTIONS options;
    UINT32 pixel_format = 0, payload_format = 0;
    UINT64 payload_bytes = 0;
    int count = 0, type = 0;
    size_t bytes_per_pixel;
    double deadline;
    memset(camera, 0, sizeof(*camera));
    if (pthread_mutex_init(&camera->mutex, NULL) != 0) return -1;
    if (!sdk_ok(GevApiInitialize(), "SDK initialisation")) goto fail;
    camera->api_ready = 1;
    memset(devices, 0, sizeof(devices));
    if (!sdk_ok(GevGetCameraList(devices, 32, &count), "Camera discovery")) goto fail;
    if (inet_pton(AF_INET, settings->camera_ip, &address) != 1) goto fail;
    if (!sdk_ok(GevOpenCameraByAddress((unsigned long)ntohl(address.s_addr),
                GevExclusiveMode, &camera->handle), "Opening camera")) goto fail;

    /* Explicit continuous acquisition; a TCP request never starts a transfer. */
    if (!sdk_ok(GevSetFeatureValueAsString(camera->handle, "AcquisitionMode", "Continuous"),
                "Setting continuous acquisition")) goto fail;
    for (int i = 0; i < settings->feature_count; i++) {
        char assignment[512];
        char *value;
        if (strlen(settings->features[i]) >= sizeof(assignment)) goto fail;
        strcpy(assignment, settings->features[i]);
        value = strchr(assignment, '=');
        if (value == NULL || value == assignment || value[1] == '\0') goto fail;
        *value++ = '\0';
        if (strcmp(assignment, "AcquisitionMode") == 0 && strcmp(value, "Continuous") != 0) {
            fprintf(stderr, "AcquisitionMode must remain Continuous.\n");
            goto fail;
        }
        if (!sdk_ok(GevSetFeatureValueAsString(camera->handle, assignment, value), assignment)) goto fail;
        fprintf(metadata, "requested.%s=%s\n", assignment, value);
        describe_feature(camera->handle, metadata, assignment);
    }
    memset(&options, 0, sizeof(options));
    if (!sdk_ok(GevGetCameraInterfaceOptions(camera->handle, &options), "Reading stream options")) goto fail;
    options.heartbeat_timeout_ms = 5000;
    options.enable_passthru_mode = 0;
    if (!sdk_ok(GevSetCameraInterfaceOptions(camera->handle, &options), "Setting stream options")) goto fail;
    if (!sdk_ok(GevGetFeatureValue(camera->handle, "Width", &type, sizeof(camera->width), &camera->width), "Reading Width") ||
        !sdk_ok(GevGetFeatureValue(camera->handle, "Height", &type, sizeof(camera->height), &camera->height), "Reading Height") ||
        !sdk_ok(GevGetFeatureValue(camera->handle, "PixelFormat", &type, sizeof(pixel_format), &pixel_format), "Reading PixelFormat") ||
        !sdk_ok(GevGetPayloadParameters(camera->handle, &payload_bytes, &payload_format), "Reading payload size")) goto fail;
    camera->format = GevGetConvertedPixelType(0, pixel_format);
    bytes_per_pixel = (size_t)GetPixelSizeInBytes(camera->format);
    if (!camera->width || !camera->height || !bytes_per_pixel ||
        (size_t)camera->width > MAX_IMAGE_BYTES / bytes_per_pixel ||
        (size_t)camera->height > MAX_IMAGE_BYTES / ((size_t)camera->width * bytes_per_pixel) ||
        payload_bytes > MAX_IMAGE_BYTES) {
        fprintf(stderr, "Invalid or excessively large image size.\n");
        goto fail;
    }
    camera->image_bytes = (size_t)camera->width * camera->height * bytes_per_pixel;
    camera->buffer_bytes = camera->image_bytes;
    if (payload_bytes > camera->buffer_bytes) camera->buffer_bytes = (size_t)payload_bytes;
    camera->latest = malloc(camera->image_bytes);
    if (camera->latest == NULL) goto fail;
    for (int i = 0; i < BUFFER_COUNT; i++) {
        camera->buffers[i] = calloc(1, camera->buffer_bytes);
        if (camera->buffers[i] == NULL) goto fail;
    }
    if (!sdk_ok(GevInitializeTransfer(camera->handle, SynchronousNextEmpty,
                camera->buffer_bytes, BUFFER_COUNT, camera->buffers), "Initialising stream")) goto fail;
    camera->transfer_ready = 1;
    if (!sdk_ok(GevStartTransfer(camera->handle, UINT32_MAX), "Starting continuous stream")) goto fail;
    if (pthread_create(&camera->thread, NULL, receive_frames, camera) != 0) goto fail;
    camera->thread_ready = 1;
    deadline = monotonic_seconds() + FIRST_FRAME_TIMEOUT;
    while (!interrupted && monotonic_seconds() < deadline) {
        int ready, failed;
        struct timespec pause = {0, 20000000};
        pthread_mutex_lock(&camera->mutex);
        ready = camera->sequence != 0;
        failed = camera->failed;
        pthread_mutex_unlock(&camera->mutex);
        if (failed) goto fail;
        if (ready) {
            const char *features[] = {"DeviceModelName", "DeviceSerialNumber", "Width", "Height",
                "PixelFormat", "AcquisitionMode", "TriggerMode", "ExposureTime", "AcquisitionFrameRate", "Gain", "BlackLevel"};
            fprintf(metadata, "camera_ip=%s\n", settings->camera_ip);
            for (size_t i = 0; i < sizeof(features)/sizeof(features[0]); i++)
                describe_feature(camera->handle, metadata, features[i]);
            if (fflush(metadata) != 0) goto fail;
            printf("Camera ready: %u x %u, pixel format 0x%08x\n", camera->width, camera->height, camera->format);
            return 0;
        }
        nanosleep(&pause, NULL);
    }
    fprintf(stderr, "No complete frame within %.0f seconds. Check the camera link and free-running settings.\n", FIRST_FRAME_TIMEOUT);
fail:
    close_camera(camera);
    return -1;
}

static int save_latest(struct camera *camera, const char *filename,
                       unsigned long long *frame, double *received, char *error, size_t error_size)
{
    unsigned char *snapshot = malloc(camera->image_bytes);
    void *pixels = snapshot;
    UINT32 save_format = camera->format;
    int converted = 0, result;
    double age;
    if (snapshot == NULL) { snprintf(error, error_size, "Out of memory"); return -1; }
    pthread_mutex_lock(&camera->mutex);
    age = monotonic_seconds() - camera->received_at;
    if (camera->failed || !camera->sequence || age > FRAME_MAX_AGE) {
        snprintf(error, error_size, "%s", camera->failed ? camera->error : "No recent complete camera frame");
        pthread_mutex_unlock(&camera->mutex);
        free(snapshot);
        return -1;
    }
    memcpy(snapshot, camera->latest, camera->image_bytes);
    *frame = camera->sequence;
    *received = camera->received_at;
    pthread_mutex_unlock(&camera->mutex);
    if (GevIsPixelTypeBayer(camera->format)) {
        size_t output_size, pixel_size;
        save_format = GevGetBayerAsRGBPixelType(camera->format);
        pixel_size = (size_t)GetPixelSizeInBytes(save_format);
        if (!pixel_size || (size_t)camera->width > MAX_IMAGE_BYTES / pixel_size ||
            (size_t)camera->height > MAX_IMAGE_BYTES / ((size_t)camera->width * pixel_size)) {
            snprintf(error, error_size, "Unsupported Bayer conversion size");
            free(snapshot);
            return -1;
        }
        output_size = (size_t)camera->width * camera->height * pixel_size;
        pixels = malloc(output_size);
        if (pixels == NULL) { snprintf(error, error_size, "Out of memory"); free(snapshot); return -1; }
        memset(pixels, GevGetPixelComponentCount(save_format) == 4 ? 0xff : 0, output_size);
        ConvertBayerToRGB(0, camera->height, camera->width, camera->format, snapshot, save_format, pixels);
        converted = 1;
    }
    result = Write_GevImage_ToTIFF((char *)filename, camera->width, camera->height, save_format, pixels);
    if (converted) free(pixels);
    free(snapshot);
    if (result <= 0) {
        unlink(filename);
        snprintf(error, error_size, "TIFF write failed (%d)", result);
        return -1;
    }
    return 0;
}

static int store_frame(struct camera *camera, const char *directory, FILE *manifest,
                       unsigned long long *number, char *reply, size_t reply_size)
{
    char final_path[PATH_MAX], temporary_path[PATH_MAX], basename[64], error[192];
    unsigned long long frame = 0;
    double received = 0, requested = monotonic_seconds();
    int n;
    snprintf(basename, sizeof(basename), "img_%08llu.tif", ++(*number));
    n = snprintf(final_path, sizeof(final_path), "%s/%s", directory, basename);
    if (n < 0 || (size_t)n >= sizeof(final_path)) { snprintf(reply, reply_size, "ERR Output path too long\n"); return -1; }
    n = snprintf(temporary_path, sizeof(temporary_path), "%s/.%s.partial.tif", directory, basename);
    if (n < 0 || (size_t)n >= sizeof(temporary_path)) { snprintf(reply, reply_size, "ERR Output path too long\n"); return -1; }
    if (save_latest(camera, temporary_path, &frame, &received, error, sizeof(error)) != 0) {
        snprintf(reply, reply_size, "ERR %s\n", error);
        return -1;
    }
    if (rename(temporary_path, final_path) != 0) {
        unlink(temporary_path);
        snprintf(reply, reply_size, "ERR Cannot finish image file: %s\n", strerror(errno));
        return -1;
    }
    if (fprintf(manifest, "%s,%llu,%.9f,%.9f\n", basename, frame, requested, received) < 0 || fflush(manifest) != 0) {
        snprintf(reply, reply_size, "ERR Image saved but frame log failed\n");
        return -1;
    }
    snprintf(reply, reply_size, "OK %s frame=%llu\n", basename, frame);
    printf("Saved %s (received frame %llu)\n", final_path, frame);
    return 0;
}

static int send_reply(int fd, const char *reply)
{
    size_t sent = 0, length = strlen(reply);
    double deadline = monotonic_seconds() + 2.0;
    while (sent < length && !interrupted && monotonic_seconds() < deadline) {
        ssize_t n = send(fd, reply + sent, length - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 100) >= 0) continue;
        }
        return -1;
    }
    return sent == length ? 0 : -1;
}

static void serve_client(int fd, struct camera *camera, const char *directory,
                         FILE *manifest, unsigned long long *number)
{
    char command[COMMAND_SIZE];
    size_t used = 0;
    int too_long = 0;
    while (!interrupted) {
        unsigned char input[1024];
        struct pollfd p = {fd, POLLIN, 0};
        int ready = poll(&p, 1, 200);
        ssize_t count;
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) continue;
        if (!(p.revents & (POLLIN | POLLHUP))) break;
        count = recv(fd, input, sizeof(input), 0);
        if (count == 0) break;
        if (count < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; break; }
        for (ssize_t i = 0; i < count && !interrupted; i++) {
            if (input[i] == '\0' || input[i] == '\n') {
                char reply[320];
                int status = 0;
                if (used && command[used-1] == '\r') used--;
                command[used] = '\0';
                if (too_long) snprintf(reply, sizeof(reply), "ERR Command too long\n");
                else if (used == 0) { used = 0; too_long = 0; continue; }
                else if (strcmp(command, "store") == 0)
                    status = store_frame(camera, directory, manifest, number, reply, sizeof(reply));
                else snprintf(reply, sizeof(reply), "ERR Expected store\n");
                if (status != 0 || strncmp(reply, "ERR", 3) == 0) fprintf(stderr, "%s", reply);
                /* The original controller sends NUL-terminated requests and never reads replies. */
                if (input[i] == '\n' && send_reply(fd, reply) != 0) return;
                used = 0;
                too_long = 0;
            } else if (!too_long) {
                if (used + 1 < sizeof(command)) command[used++] = (char)input[i];
                else too_long = 1;
            }
        }
    }
    /* An incomplete command at EOF is discarded, never interpreted as a request. */
}

static int open_listener(const struct settings *settings)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(settings->port);
    if (inet_pton(AF_INET, settings->bind_ip, &address.sin_addr) != 1 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("Camera control listener");
        close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
        "  --camera-ip ADDRESS  Camera address (default 169.254.0.3)\n"
        "  --bind ADDRESS       PC control interface (default 127.0.0.1)\n"
        "  --port PORT          TCP port (default 8080)\n"
        "  --output DIRECTORY   Parent output directory (default images)\n"
        "  --set NAME=VALUE     Set a GenICam feature before streaming; repeat as needed\n"
        "  --once               Save one frame and exit, without a controller\n"
        "  --help               Show this help\n", program);
}

int main(int argc, char **argv)
{
    struct settings settings = {"169.254.0.3", "127.0.0.1", "images", 8080, 0, 0, {0}};
    struct option options[] = {{"camera-ip", required_argument, NULL, 'c'}, {"bind", required_argument, NULL, 'b'},
        {"port", required_argument, NULL, 'p'}, {"output", required_argument, NULL, 'o'},
        {"set", required_argument, NULL, 's'}, {"once", no_argument, NULL, '1'}, {"help", no_argument, NULL, 'h'}, {0,0,0,0}};
    struct camera camera;
    struct stat info;
    struct in_addr address;
    struct sigaction action;
    char directory[PATH_MAX], path[PATH_MAX];
    FILE *metadata = NULL, *manifest = NULL;
    int option, listener = -1, result = EXIT_FAILURE, camera_ready = 0;
    unsigned long long number = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'c': settings.camera_ip = optarg; break;
        case 'b': settings.bind_ip = optarg; break;
        case 'o': settings.output = optarg; break;
        case 'p': {
            char *end;
            long port;
            errno = 0;
            port = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || port < 1 || port > 65535) { fprintf(stderr, "Invalid port\n"); return EXIT_FAILURE; }
            settings.port = (unsigned short)port;
            break;
        }
        case 's':
            if (settings.feature_count == FEATURE_COUNT || !strchr(optarg, '=') || strlen(optarg) >= 512 ||
                strchr(optarg, '\n') || strchr(optarg, '\r')) { fprintf(stderr, "Invalid or excessive feature settings\n"); return EXIT_FAILURE; }
            settings.features[settings.feature_count++] = optarg;
            break;
        case '1': settings.once = 1; break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc || inet_pton(AF_INET, settings.camera_ip, &address) != 1 ||
        inet_pton(AF_INET, settings.bind_ip, &address) != 1 || strlen(settings.output) > PATH_MAX - 100) {
        fprintf(stderr, "Check the command-line addresses and output path.\n"); return EXIT_FAILURE;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    if (mkdir(settings.output, 0755) != 0 && errno != EEXIST) { perror("Output directory"); return EXIT_FAILURE; }
    if (stat(settings.output, &info) != 0 || !S_ISDIR(info.st_mode)) { fprintf(stderr, "Output must be a directory\n"); return EXIT_FAILURE; }
    snprintf(directory, sizeof(directory), "%s/session-XXXXXX", settings.output);
    if (mkdtemp(directory) == NULL) { perror("Session directory"); return EXIT_FAILURE; }
    printf("Output: %s\n", directory);
    if (snprintf(path, sizeof(path), "%s/settings.txt", directory) >= (int)sizeof(path)) goto done;
    metadata = fopen(path, "w");
    if (metadata == NULL) { perror("Settings file"); goto done; }
    fprintf(metadata, "host_start_unix=%lld\nmode=continuous, save latest complete frame\n", (long long)time(NULL));
    if (open_camera(&camera, &settings, metadata) != 0) goto done;
    camera_ready = 1;
    if (fclose(metadata) != 0) { metadata = NULL; goto done; }
    metadata = NULL;
    if (snprintf(path, sizeof(path), "%s/frames.csv", directory) >= (int)sizeof(path)) goto done;
    manifest = fopen(path, "w");
    if (manifest == NULL) { perror("Frame log"); goto done; }
    if (fprintf(manifest, "file,received_frame_sequence,request_monotonic_s,frame_received_monotonic_s\n") < 0 || fflush(manifest) != 0) goto done;
    if (settings.once) {
        char reply[320];
        result = store_frame(&camera, directory, manifest, &number, reply, sizeof(reply)) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        printf("%s", reply);
        goto done;
    }
    listener = open_listener(&settings);
    if (listener < 0) goto done;
    printf("Listening on %s:%u\n", settings.bind_ip, settings.port);
    result = EXIT_SUCCESS;
    while (!interrupted) {
        struct pollfd p = {listener, POLLIN, 0};
        int ready = poll(&p, 1, 200), fd;
        if (ready < 0) { if (errno == EINTR) continue; result = EXIT_FAILURE; break; }
        if (ready == 0) continue;
        if (!(p.revents & POLLIN)) { result = EXIT_FAILURE; break; }
        fd = accept(listener, NULL, NULL);
        if (fd < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; result = EXIT_FAILURE; break; }
        serve_client(fd, &camera, directory, manifest, &number);
        close(fd);
    }
done:
    if (listener >= 0) close(listener);
    if (camera_ready) close_camera(&camera);
    if (metadata != NULL) fclose(metadata);
    if (manifest != NULL && fclose(manifest) != 0) result = EXIT_FAILURE;
    return result;
}
