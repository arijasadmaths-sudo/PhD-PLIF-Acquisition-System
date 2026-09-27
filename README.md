# PLIF acquisition code

Control files used for the PLIF acquisition system during the PhD experiments.

The setup used a BeagleBone Black for the laser/control timing and a Linux acquisition PC for the Teledyne DALSA Genie TS-C2500 camera. The two programs communicated over TCP/IP.

## Files

- `client.c` contains the BeagleBone-side control code. It drives the GPIO lines used by the laser/control hardware and sends a `store` request to the acquisition PC.
- `server.c` contains the PC-side TCP/IP server code retained from the acquisition setup.

The camera streamed continuously during an experiment. A `store` request told the acquisition side to save the latest completed camera frame; it did not trigger a new exposure.

## Camera software

The camera acquisition program was developed inside the Teledyne DALSA GigE-V SDK example tree and compiled against the SDK headers and libraries. The SDK itself is not included in this repository.

The acquisition computer ran Ubuntu 22.04. The standard GigE-V package available at the time failed to link because `libGevApi.so` referenced `pthread_yield`. Bristol IT raised this with Teledyne DALSA and an updated GigE-V Framework package was supplied which addressed the issue. The installation also required both 64-bit and 32-bit support because the SDK contained components for both architectures.

The camera used a dedicated wired Ethernet connection. The configuration used on the acquisition machine was:

- acquisition PC camera interface: `169.254.0.1`
- Genie TS-C2500 camera: `169.254.0.3`

After a reboot the wired camera connection had to be enabled again before the camera was visible. Camera discovery could be checked with the SDK utility:

```bash
lsgev
```

A working setup returned the camera on the dedicated interface, for example:

```text
[00:01:0D:12:78:3A]@[169.254.0.3] on enp0s31f6=[169.254.0.1]
```

The IP address in `client.c` is the controller-to-PC TCP/IP address used by the control program and is separate from the dedicated camera Ethernet addresses above.

## Repository scope

This repository is a record of the locally written acquisition/control code. It does not contain the Teledyne DALSA SDK, vendor libraries or a complete standalone camera installation. A compatible GigE-V SDK must therefore be obtained separately from Teledyne DALSA to rebuild the camera side of the system.
