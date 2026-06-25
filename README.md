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
ls -l /dev/dri/
modetest -c
drm_info
journalctl -k -g hermes-kms
sudo rmmod hermes_kms
```

## Roadmap

See [hermes-linux-virtual-display-backend-roadmap.md](hermes-linux-virtual-display-backend-roadmap.md) and [docs/driver-design.md](docs/driver-design.md).
