# PLIF acquisition code

Historical control and acquisition files for a PC, BeagleBone Black and DALSA Genie TS-C2500 camera.

- `client.c` controls the laser GPIO pins and sends a `store` message to the server.
- `server.c` receives one message, prints it and exits. Its camera call is commented out.
- `CameraCode.zip` contains camera-side source under `examples3/genicam_c_demo/`. Its ZIP directory is missing, so normal extraction fails, although most entries are recoverable.

The camera streamed continuously; `store` requested saving an available frame, not an exposure. These files alone are insufficient to run the apparatus because the camera setup and hardware wiring are absent.
