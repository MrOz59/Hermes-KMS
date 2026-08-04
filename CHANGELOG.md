# Changelog

All notable changes to Hermes-KMS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Versioning

The canonical version lives in the driver source as three defines in
[`kernel/hermes-kms/hermes_kms.c`](kernel/hermes-kms/hermes_kms.c):

```c
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 3
#define HERMES_KMS_DRIVER_PATCH 1
```

The DKMS config, the PKGBUILD, and the `GET_VERSION` ioctl all read from these,
so the module, the package, and the UAPI report the same version. A release is
cut by bumping those defines, updating this file, and pushing an annotated git
tag `vMAJOR.MINOR.PATCH`.

While the major version is `0`, the UAPI and on-disk/ioctl interfaces are still
subject to change between minor releases.

## [Unreleased]

## [0.3.1] - 2026-08-04

### Fixed

- The synthetic EDID now reaches userspace. `drm_connector_init()` attaches the
  EDID property to every connector type except VIRTUAL and WRITEBACK, and
  Hermes-KMS uses VIRTUAL, so every `drm_edid_connector_update()` failed with
  `-EINVAL` and the connector's sysfs `edid` read back empty. The failure is
  only visible with `drm.debug` enabled, so the block had been publishing
  nothing since it was added — the identity KWin looks for, the range limits and
  the per-output serial were all inert.
- The EDID range limits no longer cap the display at 75 Hz. They stated
  23-75 Hz, 15-150 kHz and 300 MHz while the driver accepts up to
  `HERMES_KMS_MAX_REFRESH_HZ` (240), so once the EDID was actually published
  they would have rejected every mode above 75 Hz, including the 1080p120 the
  CVT path synthesises correctly. They now state 240 Hz, 255 kHz and 1200 MHz;
  255 kHz is the most a 1.3 range descriptor holds without the 1.4 offset flags,
  which covers up to 1440p144.
- The EDID's detailed timing described a 1600x900 mm panel — a 72" display —
  which skews the DPI a compositor derives from it. It now describes a 24"
  1080p panel.

### Added

- Support for image-based distributions (Bazzite, Silverblue, SteamOS and other
  bootc/ostree systems), where DKMS cannot work: it rebuilds the module on the
  installed system whenever the kernel changes, and there `/usr` is read-only at
  runtime, so that moment never arrives. `packaging/bazzite/Containerfile` builds
  the module into the image instead, compiling it against the image's kernel and
  installing it to `/usr/lib/modules/<kver>/extra` the way akmods and the
  ublue-os kmod images do. It fails the build rather than ship an image whose
  module cannot be resolved, and takes an optional MOK for signing — Bazzite
  enforces Secure Boot, which will not load an unsigned out-of-tree module.
- `make modules-install` and `make install-configs`, the DKMS-free halves of an
  install: one places a built module and runs `depmod`, the other puts the module
  options, autoload entry, session-seat udev rule and seatd helper under `/usr`
  so they survive a read-only root. Both honour `DESTDIR`. `depmod` resolves
  modules through `lib/modules`, which only matches `usr/lib/modules` on a
  merged-`/usr` root, so on a staging `DESTDIR` it is skipped with a note rather
  than failing an otherwise complete build.

## [0.3.0] - 2026-07-29

### Added

- UAPI v9 multi-device prototype. The new `devices=` module parameter creates
  up to eight independent Hermes DRM cards, each with its own DRM-master domain
  and `outputs=` pipelines. Identity discovery now reports stable
  `device_index`/`device_count`, and capabilities advertise
  `HERMES_KMS_CAP_MULTI_DEVICE`.
- `scripts/vm-multi-device-test.sh` validates two simultaneous DRM masters,
  different modes, distinct owner/framebuffer/DMA-BUF channels, independent
  disconnect, and clean teardown in a disposable virtme-ng guest.
- Packaged per-device `hermes-kms-seatd@.service` instances expose private
  seatd sockets for independent compositors. A disposable VM test starts two
  brokers and two Weston DRM sessions concurrently and validates distinct
  scanout modes.

### Changed

- Output names and EDID serials are globally unique across all module-created
  devices. Existing `devices=1` behavior and UAPI structure sizes remain
  compatible.
- Requested visible widths are no longer rounded down to CVT's eight-pixel
  character-cell boundary. For example, a requested 854x480 scanout remains
  854x480 while its dumb-buffer pitch is padded independently for DMA-BUF and
  VAAPI import.
- The default single device retains the legacy platform path and host seat.
  Packaged multi-device cards receive stable `hermes-kms-1..8` udev/libseat
  assignments for independent compositors and matching virtual input devices.
- Packaged module defaults now use `initial_enabled=0`, leaving connectors
  disconnected until a streaming session owns them.

### Validated

- Two Weston DRM compositors run concurrently through separate private seat
  brokers on two Hermes cards, with independent 854x480 and 1920x1080
  scanouts as the unprivileged Hermes user
  (`scripts/vm-multi-compositor-test.sh`).
- The multi-device VM regression drives one card at the non-eight-aligned
  854x480 mode and verifies requested, active, and exported frame sizes remain
  854x480 while the XRGB8888 pitch is independently padded to 3584 bytes.

### Not yet validated

- Running two simultaneous Moonlight clients on the real host.
- Per-session input seats and audio routing; these are Hermes userspace
  concerns rather than part of the DRM UAPI.

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

[0.3.0]: https://github.com/MrOz59/Hermes-KMS/releases/tag/v0.3.0
[0.2.0]: https://github.com/MrOz59/Hermes-KMS/releases/tag/v0.2.0
[0.1.2]: https://github.com/MrOz59/Hermes-KMS/releases/tag/v0.1.2
