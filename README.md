# Hermes-KMS

Hermes-KMS is an experimental Linux DRM/KMS virtual display driver for Hermes.

The goal is to replace EVDI for Hermes with a virtual display backend that can eventually support a lower-latency DMA-BUF / PRIME path into hardware encoders.

This repository is local-only for now. No GitHub remote is configured.

## What this is

- A Linux kernel DRM/KMS driver project.
- A future EVDI replacement for Hermes.
- A virtual display target intended to expose a Hermes-owned output such as `HERMES-1`.
- A foundation for GPU-native buffer sharing and encoder import.

## What this is not

- It is not an EVDI manager.
- It is not a userspace wrapper around EVDI.
- It is not integrated into Hermes yet.
- It is not zero-copy capable yet.

## Current milestone

The current code starts the real driver path:

- out-of-tree kernel module target: `hermes_kms.ko`;
- DRM device registration skeleton;
- virtual connector using DRM/KMS helpers;
- simple display pipe;
- GEM DMA helper base for future PRIME/DMA-BUF work;
- Hermes/apps communication UAPI through DRM ioctls;
- scanout/frame metadata tracking for future DMA-BUF export;
- debug/control tool: `tools/hermes-kmsctl/hermes-kmsctl`;
- 640x480 through 3840x2160 mode range, with 1920x1080 preferred;
- no Hermes integration yet.

## Build

Install matching kernel headers first.

On CachyOS/Arch-like systems:

```bash
make
```

Clean:

```bash
make clean
```

## Local test commands

Loading an unsigned experimental kernel module can destabilize the session. Use this only on a test machine.

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko
tools/hermes-kmsctl/hermes-kmsctl version
tools/hermes-kmsctl/hermes-kmsctl caps
tools/hermes-kmsctl/hermes-kmsctl status
tools/hermes-kmsctl/hermes-kmsctl enable 1920x1080@60
ls -l /dev/dri/
modetest -c
drm_info
journalctl -k -g hermes-kms
sudo rmmod hermes_kms
```

Optional initial mode/state parameters:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1 initial_width=1920 initial_height=1080 initial_refresh_hz=60
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=0
```

## Userspace communication

Hermes-KMS exposes a small DRM ioctl UAPI in [include/uapi/drm/hermes_kms_drm.h](include/uapi/drm/hermes_kms_drm.h).

Initial ioctls:

- `DRM_IOCTL_HERMES_KMS_GET_VERSION`
- `DRM_IOCTL_HERMES_KMS_GET_CAPS`
- `DRM_IOCTL_HERMES_KMS_GET_STATUS`
- `DRM_IOCTL_HERMES_KMS_SET_OUTPUT`
- `DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME`

This gives Hermes and diagnostic tools a stable driver contract before the final DMA-BUF frame export path exists.

`GET_STATUS` now reports scanout/frame metadata:

- frame sequence counter;
- last update/enable/disable timestamps;
- framebuffer ID;
- framebuffer width/height;
- DRM fourcc format;
- modifier;
- per-plane pitch/offset.

`ACQUIRE_FRAME` currently supports metadata-only acquisition. If userspace sets `HERMES_KMS_FRAME_REQUEST_DMABUF`, the driver returns `-EOPNOTSUPP` until real DMA-BUF export is implemented. This is intentional: Hermes can code against the final API shape without Hermes-KMS falsely claiming zero-copy is ready.

Diagnostic command:

```bash
tools/hermes-kmsctl/hermes-kmsctl frame
tools/hermes-kmsctl/hermes-kmsctl frame --require-dmabuf
```

## Roadmap

See [hermes-linux-virtual-display-backend-roadmap.md](hermes-linux-virtual-display-backend-roadmap.md) and [docs/driver-design.md](docs/driver-design.md).
