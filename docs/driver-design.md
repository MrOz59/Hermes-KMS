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

## Frame and scanout tracking

The driver tracks the current simple-pipe framebuffer on every enable/update callback and exposes the metadata through `GET_STATUS`.

Tracked fields include:

- monotonically increasing frame sequence;
- update/enable/disable timestamps from `ktime_get_ns()`;
- framebuffer object ID;
- framebuffer size;
- DRM fourcc format;
- modifier;
- per-plane pitch and offset.

This does not export a DMA-BUF yet. It creates the stable state model that the future export/acquire ioctl can use without changing the basic Hermes discovery path.

## Frame acquire contract

`DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME` is the initial userspace-facing frame contract.

Current behavior:

- without flags, returns the latest tracked frame metadata;
- returns `-ENODATA` if no frame has been tracked yet;
- returns `-EOPNOTSUPP` if userspace requests DMA-BUF fds with `HERMES_KMS_FRAME_REQUEST_DMABUF`.

Future behavior:

- return one or more DMA-BUF fds;
- return an explicit sync-file fd when required;
- preserve pitch, offset, modifier, format, and sequence metadata;
- set `HERMES_KMS_FRAME_DMABUF_VALID` only when the exported fds are usable by Hermes.

This avoids a compatibility trap: Hermes can start using one ioctl shape now, while the driver remains honest about not being zero-copy capable yet.

## Initial load configuration

The module supports simple load-time parameters for test safety:

- `initial_enabled`
- `initial_width`
- `initial_height`
- `initial_refresh_hz`

This allows testing the DRM device with the connector initially disconnected, or with a specific preferred mode, before Hermes has an installer/service integration.

## Current status

The current code is a first PoC skeleton. It is meant to compile and load as a DRM driver, not yet provide the final zero-copy path.

This avoids a bad architectural trap: building a userspace wrapper around EVDI. Hermes-KMS must become its own driver.
