# Hermes-KMS roadmap

## Done

- Out-of-tree DRM/KMS module with a virtual `HERMES-1` connector and synthetic
  EDID.
- Explicit CRTC/encoder/plane modeset with a software vblank timer
  (60/120/144 Hz, lockdep-clean, deterministic pacing).
- Cursor plane, separate `WAIT_UPDATE` / `ACQUIRE_CURSOR` capture stream, and
  sequence-safe `FB_DAMAGE_CLIPS` damage tracking. Cursor-only commits do not
  wake primary-frame consumers; cursor acquisitions carry position, hotspot,
  clipping, DMA-BUF and explicit-fence metadata, and a skipped primary sequence
  falls back to whole-frame damage.
- Render node for masterless, zero-copy frame consumption; all ioctls are
  `DRM_RENDER_ALLOW`.
- DMA-BUF + sync_file export of the tracked scanout framebuffer.
- Owner-fd session lifecycle, stable output identity, strict atomic check.
- DRM ioctl UAPI (version/identity/caps/status/set-output/session access,
  frame/cursor acquire, frame/combined wait, and metrics) and debugfs telemetry.
- End-to-end zero-copy validated on VAAPI (XRGB8888, linear).
- DKMS + Arch/CachyOS packaging.
- UAPI v9 multi-device prototype with independent DRM-master domains and a
  disposable two-device VM regression.
- Packaged private seat-broker instances and a VM regression with two
  unprivileged Weston DRM compositors scanning out concurrently on separate
  Hermes cards.
- UAPI v10 host-compatible automatic session pools, role-aware udev policy,
  broker auto-start, and a one-command service-user setup path.
- UAPI v11 generic session-capability handoff. Output owners can authorize
  separate capture fds with opaque, revocable tokens without any Hermes, Steam,
  executable-name, UID or process-relationship rule in the driver.
- Native/ILP32 ABI regression coverage for every public struct and ioctl.
- Installable syscall-note UAPI and MIT-licensed, application-neutral session
  helper for external consumers; the kernel contract contains no Hermes/Steam
  process identity rule.
- Runtime card creation and removal through configfs
  (`/sys/kernel/config/hermes-kms/`), matching the interface vkms adopted
  upstream. Cards no longer have to be drawn from a pool fixed at module load,
  and a project that only wants a virtual display can create one without
  writing any ioctl code.

## Next

- Real-host validation of the two concurrency models: KWin adoption and
  persistence for `devices=1 outputs=N`, plus two simultaneous Moonlight
  sessions for `devices=N outputs=1`. Disposable VM tests already validate
  simultaneous owners/DRM masters, two concurrent Weston compositors, distinct
  framebuffer/DMA-BUF channels, independent disconnect, and clean unload.
- NVENC/AMF DMA-BUF import validation (VAAPI is validated).
- NV12/P010 scanout and HDR. The compositor composes in RGB and the encoder
  does RGB→NV12 on the real GPU today, so this is an optimization, not a
  blocker.
- Encoder consumption of the damage rectangle for partial-frame encode.
- Wider compositor coverage (wlroots/GNOME beyond KWin).
- Real-host validation of separate cursor composition, including clipped cursor
  edges, hotspot placement, visibility transitions, and in-place image updates.
- Compositor recovery handling beyond owner-fd disconnect and hotplug.

## Out of scope (for now)

- A real DRM writeback connector. Doing this properly needs writeback-connector
  plumbing, not placeholder flags.
- Managing EVDI. EVDI stays a parallel fallback in Hermes, not something
  Hermes-KMS wraps.
- Deciding which application or sandbox should receive a session token. That is
  userspace policy transported over the consuming project's trusted IPC.

See [driver-design.md](driver-design.md) for the architecture.
