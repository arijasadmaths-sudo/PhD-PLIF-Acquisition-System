# PLIF

Historical control and acquisition material for the thesis PLIF apparatus (PC, BeagleBone Black and DALSA GenieCam TS-C2500 camera).

The camera streamed continuously. A network message requested that an available frame be saved; it was not a sensor exposure trigger. The stream was not phase-locked to the laser pulses, so this archive does not establish exposure-level synchronisation or a measured pulse-to-frame delay.

## Archive status

- `client.c` and `server.c` are historical network-control fragments, not a complete runnable acquisition release. The deposited server handles one received message and exits; its camera call is commented out.
- The deposited `CameraCode.zip` fails ZIP integrity testing. It is retained for provenance, not offered as a verified installable package. A complete, tested replacement is still needed.
- Camera/SDK configuration, hardware wiring, pulse timing and frame-age logs must be supplied and checked before attempting acquisition. No hardware execution was performed in this documentation review.

The experimental method and its timing limitations are described in Chapter 2 of the thesis. Image processing and analysis are maintained separately in [plume-thesis-code](https://github.com/arijasadmaths-sudo/plume-thesis-code); that repository may require access permission.
