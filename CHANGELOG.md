# Changelog

All notable changes to Hermes-KMS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Versioning

The canonical version lives in the driver source as three defines in
[`kernel/hermes-kms/hermes_kms.c`](kernel/hermes-kms/hermes_kms.c):

```c
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 4
#define HERMES_KMS_DRIVER_PATCH 0
```

The DKMS config, the PKGBUILD, and the `GET_VERSION` ioctl all read from these,
so the module, the package, and the UAPI report the same version. A release is
cut by bumping those defines, updating this file, and pushing an annotated git
tag `vMAJOR.MINOR.PATCH`.

While the major version is `0`, the UAPI and on-disk/ioctl interfaces are still
subject to change between minor releases.

## [Unreleased]

### Added

- Scanout modifier pass-through. The primary plane now implements
  `format_mod_supported` and accepts any DRM format modifier, because the driver
  only latches and re-exports the compositor's buffer and never samples a pixel.
  Previously the DRM core's linear-only default rejected a tiled or compressed
  scanout at atomic check time, forcing the compositor to render into a detiled
  target. The new `scanout_modifiers=` module parameter additionally publishes up
  to 15 extra layouts through the primary plane's `IN_FORMATS`, which is what an
  IN_FORMATS-driven compositor (KWin, wlroots) negotiates from; the default
  remains linear-only, so behaviour is unchanged until it is set. The cursor
  plane deliberately stays linear-only.
- UAPI v11 generic session-capability handoff. An output owner obtains a random,
  opaque 128-bit token and can authorize separate capture fds with an atomic
  token/session/output bind. Closing or disabling the owner revokes the session.
  The mechanism is deliberately independent of Hermes, Steam, executable names,
  UIDs and process relationships so other projects can use the driver unchanged.
- UAPI v11 separate cursor capture. `HERMES_KMS_CAP_CURSOR_CAPTURE`,
  `GET_IDENTITY.cursor_plane_id`, `WAIT_UPDATE`, and `ACQUIRE_CURSOR` expose
  cursor position, hotspot, clipped destination/source geometry, visibility,
  image/state sequences, ARGB8888 DMA-BUFs, and explicit sync_file fences
  without advancing the primary-frame stream on cursor-only motion.
- The cursor probe can bind through the generic session helper and exercise the
  cursor wait/acquire, fence, DMA-BUF mapping, and cursor-only sequence contract
  at runtime.
- `vblank_count` and `vblank_overrun_count` are now public UAPI metrics, using
  previously reserved words without changing the metrics struct size.
- CI builds the kernel module and optional diagnostic tools, checks packaged
  configuration syntax, and compiles/runs `tests/uapi-abi.c` for native x86-64
  and i386 userspace ABIs.
- UAPI v10 host-compatible session pools. `session_devices=N` creates one
  seat0 host card plus N private session cards and reports explicit
  host/session roles and stable private-seat indices without changing the
  identity struct size.
- `hermes-kms-setup` configures the recommended four-session pool and assigns
  broker sockets to the Hermes service uid with one privileged invocation. It
  also removes legacy manual broker enablement and stops instances outside the
  configured pool.
- `scripts/vm-session-pool-test.sh` validates host/private roles, stable
  indices, udev seat assignment, and automatic systemd broker wants.
- A dedicated polkit action gives the one-click setup a scoped, localized
  authentication prompt.
- `scripts/vm-setup-helper-test.sh` validates persistent setup, exact broker
  restarts, and the safe reboot-required upgrade path.
- `scripts/vm-systemd-broker-test.sh` validates the actual packaged systemd
  unit, its hardening, and configured-user socket ownership.

### Changed

- The default `make` target builds only the kernel module. Developers can use
  `make full` to opt into diagnostic tools and their libdrm/VAAPI/GBM/EGL/GL
  dependencies.
- Active-session status, frame acquisition, waits and metrics now require the
  owner fd or a fd bound with the generic UAPI v11 session capability. A
  root-only `insecure_legacy_unbound_access=1` migration switch can restore old
  behavior for diagnostics, but is disabled by default.
- External consumers are documented as a first-class integration path. They
  validate a candidate node with core `DRM_IOCTL_VERSION` before private ioctls,
  and can install the syscall-note UAPI plus the application-neutral,
  MIT-licensed session helper without installing Hermes application code.
- The Arch/CachyOS package now depends on `seatd`, installs all runtime files
  together, and defaults to one host card plus four disconnected private
  cards. No scanout memory is allocated until a client owns a card.
- Role-aware udev rules keep the host card on seat0 and automatically start
  only the broker instances corresponding to private cards.
- `make dkms-install` now installs module-load/modprobe defaults, udev rules,
  broker units, and the setup helper instead of leaving those as manual steps.
  It detects an already-loaded older module and asks for a reboot instead of
  implying that the new DKMS build is active.
- Private broker sockets can be owned directly by the configured Hermes user,
  removing the logout/login and supplementary `seat` group requirement.

### Validated

- Public UAPI sizes, alignment-sensitive offsets, and encoded ioctl values are
  identical for newly compiled x86-64 and i386 clients under UAPI v11,
  including the cursor acquire and dual-stream wait structures.
- The existing UAPI v7, multi-output, explicit `devices=N`, simultaneous DRM
  master, DMA-BUF, and two-Weston VM regressions still pass under UAPI v10.

### Fixed

- GNOME/Mutter can now use the Hermes cursor plane. The driver no longer sets
  `DRIVER_CURSOR_HOTSPOT`, whose opt-in client capability caused the DRM core to
  hide the plane from ordinary compositors; cursor coordinates remain standard
  universal-plane coordinates and the optional hotspot metadata stays invalid
  unless a future protocol supplies it explicitly.
- Cursor framebuffers smaller than a display mode are no longer rejected by
  the DRM core. `mode_config.min_width/min_height` describe every framebuffer,
  so setting them to the 640x480 minimum mode made Mutter's 256x256 ARGB cursor
  fail both `ADDFB2` and legacy `ADDFB` with `-EINVAL` before `fb_create`.
  Framebuffer minima are now 1x1 while connector modes and `SET_OUTPUT` retain
  the 640x480 limit; the imported-scanout regression tool pins this exact case.
- Active primary and cursor planes now provide the no-op `atomic_update` and
  `atomic_disable` hooks required by `drm_atomic_helper_commit_planes()`. A
  previous bookkeeping refactor left those callable slots NULL and could jump
  to address zero on the first active commit.
- Capture damage is no longer reused unsafely after a consumer skips a frame.
  Consecutive new sequences may carry the compositor's clip, re-acquiring the
  same sequence returns a valid empty rectangle, first/gapped acquisitions fall
  back to the full frame, and cursor-only atomic commits no longer advance the
  primary-frame sequence.
- Primary and cursor state from one atomic commit are latched before a single
  waiter wake. `WAIT_UPDATE` returns a coherent current snapshot while retaining
  independent frame and cursor sequence spaces, so consumers never have to
  infer that unrelated sequence numbers form one transaction ID.
- DMA-BUF and sync_file export revalidates the selected session and buffer
  generation before installing fds. A replacement race returns `-ESTALE` for a
  bounded latest-state retry instead of pairing stale metadata with a new fd.
- Frame and dual-stream waits revalidate authorization on timeout and poll
  paths, so session revocation wins with `-EACCES` rather than being mistaken
  for an ordinary timeout.
- Public structs now use explicitly aligned 64-bit fields and `GET_STATUS`
  replaces LP64-only implicit padding with a named reserved word. Newly compiled
  32-bit and 64-bit userspace therefore uses the same ioctl encodings.
- The module builds again on Linux 7.2. That release renamed
  `struct drm_atomic_state` to `struct drm_atomic_commit` — the object was
  always one commit's worth of state, never the device's entire state — and
  retyped every atomic helper callback with it. The four CRTC and four plane
  callbacks here still took the old type, so DKMS failed with 15
  `-Wincompatible-function-pointer-types` and `-Wincompatible-pointer-types`
  errors and left users on 7.2 with no virtual display at all (Hermes#26).
  - The callbacks now use the current upstream name, and a compat `#define`
    maps it back to `drm_atomic_state` on older kernels. Nothing else about
    those callbacks changed between the two spellings.
  - Which name to use is decided by grepping the target kernel's own
    `include/drm/drm_atomic.h` from kbuild rather than by comparing
    `LINUX_VERSION_CODE`, so a tree that backported the rename builds too; the
    version comparison is the fallback for when that header cannot be read.
  - Verified both ways: against the 7.1 headers the driver builds warning-free
    as before, and against a copy of them with the rename applied it builds
    warning-free where the unpatched source produces exactly the 15 reported
    errors.
- Scanning out of a buffer the driver did not allocate no longer produces a
  DMA-BUF the consumer cannot import. A compositor that renders on the real GPU
  can import that buffer into this device and scan out of it directly rather
  than drawing into a dumb buffer we allocated — Mutter does this where KWin
  does not. `gem_prime_import` then hands us a shmem object with
  `import_attach` set and no shmem file behind it, and `ACQUIRE_FRAME`
  re-exported that object with `drm_gem_prime_export()`, producing a DMA-BUF
  whose pages cannot be pinned.
  - The consumer only discovered this on attach, far from the cause:
    `drm_gem_map_attach()` → `drm_gem_shmem_pin_locked()` warns on
    `drm_gem_is_imported()`, `drm_gem_get_pages()` rejects the NULL `filp` with
    `-EINVAL`, and the user sees `EGL_BAD_ALLOC` out of `eglCreateImage()` — a
    black stream with working audio and input (Hermes#22).
  - An imported scanout object now hands back the DMA-BUF it was imported from,
    which is the buffer the GPU already understands.
  - `tools/hermes-imported-scanout-test` builds the situation without a GPU or
    a compositor, using udmabuf, and reads the frame back. Before the fix it is
    killed touching the returned buffer; after it, the data arrives intact.
- Buffers are no longer refused by GPUs that pad a linear surface's height.
  An importer recomputes the surface layout from the geometry and rejects a
  buffer smaller than that layout needs rather than reading past its end —
  radeonsi does this in `si_texture_from_winsys_buffer()`, comparing the
  buffer against `surface.total_size`, which `ac_surface.c` derives from a
  padded height (`surf_slice_size = pitch * surf_height * bpe`). The dumb
  buffer covered only `pitch x height` rounded to a page, so it imported on
  hardware that pads by nothing and was rejected on hardware that pads.
  This stayed invisible because the common resolutions have heights that are
  already aligned — 720, 1080 and 1440 all are — and surfaced on an unusual
  one: a client at 1600x1068 fell 24576 bytes short.
  - The backing height is now padded to 16 rows. The visible height is
    untouched and only the allocation grows, by at most 15 rows: 60 KiB at
    1080p, 230 KiB on a 4K buffer, and nothing at all for the resolutions
    that were already aligned.
- A stale `/etc/modprobe.d/hermes-kms.conf` no longer silently keeps virtual
  outputs connected at boot. The package installs its default to
  `/usr/lib/modprobe.d/hermes-kms.conf`, but `/etc/modprobe.d` overrides
  `/usr/lib/modprobe.d`, so an `initial_enabled=1` written there by an older
  package — or by hand, following older setup instructions — masks the shipped
  `initial_enabled=0`, and no upgrade can undo it because the package does not
  own that file. A connector then comes up connected before Hermes owns it and
  the compositor extends the desktop onto a virtual output nobody streams to,
  which the user sees as a black screen in display settings.
  - The package now reports the offending file on install and upgrade, with the
    two ways to resolve it.
  - The driver logs a `drm_warn` at probe when `initial_enabled` is set, naming
    `/etc/modprobe.d` as the place to look, so the cause shows up in `dmesg`
    instead of only in display settings.

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
