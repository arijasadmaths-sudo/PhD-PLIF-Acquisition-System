/* Software-only SDK double. Its image files are deliberately NOT TIFF files. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gevapi.h"
#include "SapX11Util.h"
#include "FileUtil.h"

static UINT8 *buffer;
static int owned, running;
static unsigned int generated;
static UINT32 pixel_format;
static GEV_BUFFER_OBJECT image;
static int handle_object;

static void delay_ms(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

static void log_call(const char *call)
{
    const char *path = getenv("MOCK_LOG");
    if (path != NULL) {
        FILE *file = fopen(path, "a");
        if (file != NULL) { fprintf(file, "%s\n", call); fclose(file); }
    }
}

GEV_STATUS GevApiInitialize(void)
{
    buffer = NULL; owned = running = 0; generated = 0;
    pixel_format = getenv("MOCK_MONO") ? TEST_MONO : TEST_BAYER;
    log_call("initialise"); return 0;
}
GEV_STATUS GevApiUninitialize(void) { log_call("uninitialise"); return 0; }
GEV_STATUS GevGetCameraList(GEV_DEVICE_INTERFACE *d, int capacity, int *count)
{
    if (capacity < 1) return -1;
    d[0].ipAddr = 0xa9fe0003u; *count = 1; return 0;
}
GEV_STATUS GevOpenCameraByAddress(unsigned long address, int mode, GEV_CAMERA_HANDLE *handle)
{
    if (address != 0xa9fe0003ul || mode != GevExclusiveMode) return -2;
    *handle = &handle_object; return 0;
}
GEV_STATUS GevCloseCamera(GEV_CAMERA_HANDLE *handle) { *handle = NULL; log_call("close"); return 0; }
GEV_STATUS GevGetCameraInterfaceOptions(GEV_CAMERA_HANDLE handle, GEV_CAMERA_OPTIONS *options)
{
    (void)handle; memset(options, 0, sizeof(*options)); return 0;
}
GEV_STATUS GevSetCameraInterfaceOptions(GEV_CAMERA_HANDLE handle, GEV_CAMERA_OPTIONS *options)
{
    (void)handle; return options->enable_passthru_mode == 0 ? 0 : -1;
}
GEV_STATUS GevSetFeatureValueAsString(GEV_CAMERA_HANDLE handle, const char *name, const char *value)
{
    (void)handle;
    if (strcmp(name, "BadFeature") == 0) return -3;
    if (strcmp(name, "AcquisitionMode") == 0 && strcmp(value, "Continuous") != 0) return -4;
    log_call(name); return 0;
}
GEV_STATUS GevGetFeatureValueAsString(GEV_CAMERA_HANDLE handle, const char *name, int *type, int size, char *value)
{
    (void)handle; (void)name; *type = 0;
    if (size <= 0) return -1;
    snprintf(value, (size_t)size, "mock-value"); return 0;
}
GEV_STATUS GevGetFeatureValue(GEV_CAMERA_HANDLE handle, const char *name, int *type, int size, void *value)
{
    UINT32 result;
    (void)handle; *type = 0;
    if (size != (int)sizeof(UINT32)) return -1;
    if (strcmp(name, "Width") == 0) result = 8;
    else if (strcmp(name, "Height") == 0) result = 6;
    else if (strcmp(name, "PixelFormat") == 0) result = pixel_format;
    else return -1;
    memcpy(value, &result, sizeof(result)); return 0;
}
GEV_STATUS GevGetPayloadParameters(GEV_CAMERA_HANDLE handle, UINT64 *size, UINT32 *format)
{
    (void)handle; *size = 48; *format = pixel_format; return 0;
}
GEV_STATUS GevInitializeTransfer(GEV_CAMERA_HANDLE handle, int mode, UINT64 size, UINT32 count, UINT8 **buffers)
{
    (void)handle;
    if (mode != SynchronousNextEmpty || size < 48 || count < 2) return -5;
    buffer = buffers[0]; log_call("transfer_init"); return 0;
}
GEV_STATUS GevStartTransfer(GEV_CAMERA_HANDLE handle, UINT32 count)
{
    (void)handle;
    if (getenv("MOCK_START_FAIL")) return -6;
    if (count != UINT32_MAX || running) return -7;
    running = 1; log_call("continuous_start"); return 0;
}
GEV_STATUS GevAbortTransfer(GEV_CAMERA_HANDLE handle) { (void)handle; running = 0; log_call("abort"); return 0; }
GEV_STATUS GevFreeTransfer(GEV_CAMERA_HANDLE handle)
{
    (void)handle; log_call(owned ? "ERROR_buffer_still_owned" : "free_transfer"); return owned ? -8 : 0;
}
GEV_STATUS GevWaitForNextImage(GEV_CAMERA_HANDLE handle, GEV_BUFFER_OBJECT **result, UINT32 timeout)
{
    (void)handle; (void)timeout; *result = NULL;
    delay_ms(30);
    if (!running || owned) return -9;
    if (getenv("MOCK_STALE") && generated >= 3) { delay_ms(70); return -10; }
    generated++;
    image.status = getenv("MOCK_INCOMPLETE") ? 1 : 0;
    image.w = getenv("MOCK_BAD_GEOMETRY") ? 7 : 8;
    image.h = 6; image.format = pixel_format; image.address = buffer;
    memset(buffer, (int)(generated % 200 + 1), 48);
    owned = 1; *result = &image; return 0;
}
GEV_STATUS GevReleaseImage(GEV_CAMERA_HANDLE handle, GEV_BUFFER_OBJECT *entry)
{
    (void)handle;
    if (!owned || entry != &image) return -11;
    /* Immediate reuse exposes any accidental saving from a released SDK pointer. */
    memset(buffer, 0xee, 48); owned = 0; return 0;
}
UINT32 GevGetConvertedPixelType(int bayer, UINT32 format) { (void)bayer; return format; }
int GevIsPixelTypeBayer(UINT32 format) { return format == TEST_BAYER; }
UINT32 GevGetBayerAsRGBPixelType(UINT32 format) { (void)format; return TEST_RGB; }
UINT32 GevGetPixelComponentCount(UINT32 format) { return format == TEST_RGB ? 3 : 1; }
UINT32 GetPixelSizeInBytes(UINT32 format) { return format == TEST_RGB ? 3 : 1; }
void ConvertBayerToRGB(int method, UINT32 height, UINT32 width, UINT32 input_format, void *input, UINT32 output_format, void *output)
{
    unsigned char *in = input, *out = output;
    (void)method; (void)input_format; (void)output_format;
    for (size_t i = 0; i < (size_t)height * width; i++)
        for (size_t c = 0; c < 3; c++) out[3*i+c] = in[i];
}
int Write_GevImage_ToTIFF(char *name, UINT32 width, UINT32 height, UINT32 format, void *pixels)
{
    size_t bytes = (size_t)width * height * GetPixelSizeInBytes(format);
    FILE *file = fopen(name, "wb");
    if (file == NULL) return -1;
    delay_ms(80);
    fprintf(file, "MOCK_NOT_TIFF %u %u %u\n", width, height, GetPixelSizeInBytes(format));
    if (getenv("MOCK_WRITE_FAIL")) { fclose(file); return -12; }
    if (fwrite(pixels, 1, bytes, file) != bytes) { fclose(file); return -13; }
    if (fclose(file) != 0) return -14;
    return (int)bytes;
}
void _CloseSocketAPI(void) { log_call("socket_api_close"); }
