# Changelog

All notable changes to Hermes-KMS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Versioning

The canonical version lives in the driver source as three defines in
[`kernel/hermes-kms/hermes_kms.c`](kernel/hermes-kms/hermes_kms.c):

```c
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 2
#define HERMES_KMS_DRIVER_PATCH 0
```

The DKMS config, the PKGBUILD, and the `GET_VERSION` ioctl all read from these,
so the module, the package, and the UAPI report the same version. A release is
cut by bumping those defines, updating this file, and pushing an annotated git
tag `vMAJOR.MINOR.PATCH`.

While the major version is `0`, the UAPI and on-disk/ioctl interfaces are still
subject to change between minor releases.

## [0.2.0] - 2026-07-28

### Added

- Prototype support for 1–8 independent virtual outputs on one DRM device
  (`outputs=`, default 1 for compatibility), each with its own connector, CRTC,
  planes, software vblank timer, owner session, framebuffer channel, waitqueue,
  metrics, and DMA-BUF export cache. Multiple outputs remain opt-in until the
  Hermes host integration manages every connector.
- UAPI v8 `SELECT_OUTPUT` binding. A new DRM fd remains bound to output 0 for
  compatibility; updated clients bind one fd per concurrent output.
- Stable per-output identities (`HERMES-1`, `HERMES-2`, ...) and distinct EDID
  serials so compositors can persist each virtual monitor separately.
- `hermes-kmsctl --output N` and `hermes-kmsctl outputs`.
- Disposable virtme-ng tests for concurrent modeset, ownership, DMA-BUF
  export, independent disconnect (`scripts/vm-multi-output-test.sh`), and
  compatibility with an unmodified v0.1.2 client
  (`scripts/vm-uapi-v7-compat-test.sh`).

### Validation

- Builds against CachyOS kernel `7.0.9-1-cachyos` with kbuild `W=1`.
- The control tool builds with `-Wall -Wextra`.
- Three consecutive virtme-ng runs validated two simultaneous atomic modesets
  at different resolutions, distinct owner fds and framebuffers, DMA-BUF +
  sync_file export from both outputs, independent disconnect, clean unload,
  and no kernel splat.
- The unmodified v0.1.2 `hermes-kmsctl` binary was tested against UAPI v8: its
  existing ioctls still operate on `HERMES-1`, while `HERMES-2` remains
  independent.
- Existing VM regressions pass after the refactor: software vblank pacing at
  60/120/144 Hz with zero overruns, plus a 5-second/4-thread export stress
  (4,652,791 acquires, 299 flips, zero errors/splats, clean unload). The short
  stress run did not use KASAN or SLUB debug.
- KWin compositor adoption/recovery and real encoder integration are not yet
  runtime-validated for multiple outputs.

## [0.1.2] - 2026-06-30

First tagged release. The driver is functional and validated end-to-end on
KWin + VAAPI, but this is still an early `0.x` release — expect rough edges,
narrow compositor/encoder coverage, and bugs in untested configurations. See
the warning at the bottom of this entry.

### Added

- Out-of-tree DRM/KMS kernel module (`hermes_kms.ko`) exposing a virtual
  `HERMES-1` connector with a synthetic EDID.
- Explicit CRTC/encoder/plane modeset driven by a software vblank timer at
  60/120/144 Hz for deterministic frame pacing (lockdep-clean).
- Render node (`DRIVER_RENDER`) so a capture consumer can pull frames without
  taking DRM master; every Hermes ioctl is `DRM_RENDER_ALLOW`.
- Zero-copy frame consumption: DMA-BUF + `sync_file` export of the tracked
  scanout framebuffer, with no CPU readback (~8 us/frame `ACQUIRE_FRAME` on
  KWin at 720p, constant across resolutions).
- Cursor plane and `FB_DAMAGE_CLIPS` damage tracking.
- Owner-fd session lifecycle, stable output identity, and strict atomic check.
- DRM ioctl UAPI: version / identity / caps / status / set-output / acquire /
  wait / metrics, documented in
  [`include/uapi/drm/hermes_kms_drm.h`](include/uapi/drm/hermes_kms_drm.h).
- debugfs telemetry for the session and frame pipeline.
- End-to-end zero-copy path validated on VAAPI (XRGB8888, linear).
- DKMS packaging plus Arch/CachyOS PKGBUILD and udev/modprobe integration.
- Reference userspace tools: `hermes-kmsctl` and `hermes-kms-import-check`.

### Known limitations

- Only the **VAAPI** import path is validated end-to-end. NVENC/AMF DMA-BUF
  import is not yet tested.
- Only **RGB** scanout (XRGB8888, linear). NV12/P010 and HDR are not
  implemented; the encoder does RGB→NV12 on the real GPU today.
- Validated primarily on **KWin**; wlroots/GNOME and other compositors are
  largely untested.
- Compositor recovery is limited to owner-fd disconnect and hotplug handling.
- No real DRM writeback connector (intentionally out of scope for now).

> **Early-release warning.** This is one of the first releases of Hermes-KMS.
> Even though it is tagged as a release, the driver is in early development and
> may contain bugs and unexpected behavior, including on untested compositors,
> encoders, and kernel versions. It is a kernel module — a crash can take down
> your graphics session. Do not run it on a machine where you cannot tolerate
> an unstable display stack, and please report issues you hit.

[0.2.0]: https://github.com/MrOz59/Hermes-KMS/releases/tag/v0.2.0
[0.1.2]: https://github.com/MrOz59/Hermes-KMS/releases/tag/v0.1.2
