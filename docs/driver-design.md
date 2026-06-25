# Hermes-KMS driver design

Hermes-KMS is intended to be a Linux DRM/KMS virtual display driver that can replace EVDI for Hermes.

The target is not to manage EVDI. EVDI remains a temporary Hermes fallback until Hermes-KMS is mature enough.

## Target architecture

```text
Compositor / game
    ↓
HERMES-1 DRM/KMS virtual connector
    ↓
GPU-backed framebuffer / GEM object
    ↓
PRIME / DMA-BUF export
    ↓
Hermes userspace
    ↓
VAAPI / future NVENC import
    ↓
stream
```

## Development order

1. Buildable out-of-tree kernel module.
2. Register a DRM device safely.
3. Expose a virtual connector named by DRM as `Virtual-*` initially, with user-facing target `HERMES-1`.
4. Support simple modes such as `1920x1080@60`.
5. Use GEM/DMA helpers as the first buffer-management base.
6. Add explicit Hermes control/diagnostic interface.
7. Implement PRIME/DMA-BUF frame export path.
8. Measure whether the encoder path is actually zero-copy.

## Communication with Hermes

Hermes-KMS should be controlled through explicit DRM ioctls, not by scraping logs or guessing connector names.

The first UAPI lives in `include/uapi/drm/hermes_kms_drm.h` and provides:

- version discovery;
- capability discovery;
- current output status;
- requested output enable/disable and preferred mode.

This is deliberately small. It can be safely extended with append-only structs and new ioctls for:

- frame queue descriptors;
- DMA-BUF export/acquire;
- explicit sync fences;
- latency counters;
- per-client ownership/session IDs.

## Current status

The current code is a first PoC skeleton. It is meant to compile and load as a DRM driver, not yet provide the final zero-copy path.

This avoids a bad architectural trap: building a userspace wrapper around EVDI. Hermes-KMS must become its own driver.
