# PLIF camera acquisition

PC-side receiver for a Teledyne DALSA Genie TS-C2500 and the BeagleBone control system. The receiver uses the GigE-V SDK, keeps the camera acquiring continuously, and saves a TIFF when it receives `store`. The request saves the latest complete frame; it does not trigger an exposure. It runs without a display window.

## 1. Install the camera SDK

Install a Teledyne DALSA GigE-V Framework for Linux package compatible with the acquisition computer, including its GenICam libraries, C examples and TIFF support. A C compiler, make, Python 3 and the libtiff development files are required. Follow the SDK installation instructions for the library paths and any further dependencies.

The Bristol acquisition PC used Ubuntu 22.04. Its original SDK package failed to link because `libGevApi.so` referenced `pthread_yield`; Teledyne supplied an updated framework. Use a compatible package rather than trying to fix that linker error in this program. The historical installation also enabled the 32-bit dependencies needed by some SDK components. The receiver itself must be built for the architecture of the installed libraries.

Obtain the SDK from [Teledyne DALSA](https://www.teledynedalsa.com/en/support/downloads-center/software-development-kits/132/). Vendor libraries and examples are not distributed here.

## 2. Connect and check the camera

The laboratory camera link used a dedicated wired Ethernet interface:

| Device | Address |
| --- | --- |
| PC camera interface | `169.254.0.1` |
| Camera | `169.254.0.3` |

Enable that wired connection after a reboot and check discovery:

```sh
lsgev
```

The camera and PC interface must be on the same subnet. Keep this camera link separate from the controller-to-PC connection. Close other camera applications before starting the receiver, which requests exclusive access.

Use the SDK tools to check the image, free-running acquisition, pixel format, exposure and frame rate. The experimental settings were 10 frames/s and 100000 microseconds exposure. Keep the image format, gain and other settings consistent with the calibration being used. This program does not apply intensity scaling or automatic exposure adjustments.

## 3. Build

First check that the SDK's unmodified `genicam_c_demo` example builds. Then, from this repository:

```sh
python3 build_camera.py --sdk-example "$HOME/DALSA/GigeV/examples3/genicam_c_demo"
```

Some SDK installations use `examples` instead of `examples3`; pass the directory present on your machine. The script creates a new `plif_camera` directory beside the example, compiles `server.c` with the example makefile, and places the executable at `build/plif_camera`. It leaves the original example untouched. For a later rebuild, use a new sibling directory with `--build-dir`.

The original SDK headers, utility functions and library paths are required. The small headers under `tests/sdk` are test doubles and must not be used to build the real camera program.

## 4. Save a test image

With the laser disabled, point the camera at a normally illuminated target and run:

```sh
./build/plif_camera --camera-ip 169.254.0.3 --output images --once
```

The program waits for a complete frame, saves one TIFF and exits. Check that the TIFF opens correctly and contains the expected image before collecting experimental data.

Each run creates a separate `session-*` output directory. It contains numbered TIFF files, `settings.txt` with available camera feature readbacks, and `frames.csv` with the received-frame sequence and host monotonic times for the request and frame reception. These are host times, not exposure timestamps or laser synchronisation measurements.

## 5. Receive storage requests

Start the receiver on the PC address reachable from the BeagleBone. The address in the retained `client.c` is `169.254.90.110`, distinct from the camera-link addresses above:

```sh
./build/plif_camera --camera-ip 169.254.0.3 --bind 169.254.90.110 --port 8080 --output images
```

Use the address actually assigned to the PC control interface. Without `--bind`, the receiver listens only on `127.0.0.1`. The control socket has no authentication; use it only on the trusted apparatus network and restrict access to port 8080.

The existing controller sends NUL-terminated `store` messages. The receiver accepts these repeatedly and accepts a new connection after a client disconnects. Newline-terminated `store` messages are also accepted and receive an `OK` or `ERR` reply. NUL-terminated requests receive no reply, matching the original controller, which does not read responses.

To test the network path without running the laser controller:

```sh
python3 request_frame.py --host 169.254.90.110 --count 3 --interval 6
```

Stop the receiver with Ctrl+C. A missing, incomplete or stale frame is not saved as a new image. Errors are printed on the PC terminal.

Individual GenICam settings can be supplied before streaming, for example:

```sh
./build/plif_camera --once --set ExposureTime=100000
```

Use feature names and values supported by the camera's XML. Unsupported writes stop the program rather than being silently ignored. Acquisition mode is set to `Continuous`; other camera settings are left unchanged unless supplied with `--set`.

## Controller and checks

`client.c` is the retained BeagleBone GPIO program and is not changed by the camera receiver. Its pin assignments and laser-control behaviour are specific to the original apparatus. It is not a general-purpose safety controller. Establish camera operation with the laser disabled; laser operation requires the apparatus interlocks and approved laboratory procedure.

`archive/server_socket_test.c` is the earlier socket-only receiver, not the camera application.

Run the software checks with:

```sh
python3 tests/test_camera.py
```

They compile the receiver against a simulated SDK and exercise frame copying, TIFF-write error handling and TCP requests. They do not test the vendor runtime, camera, network hardware or laser. A successful build with the installed SDK and the test-image check above are still needed on the acquisition PC.
