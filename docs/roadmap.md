# Hermes-KMS roadmap

## Done

- Out-of-tree DRM/KMS module with a virtual `HERMES-1` connector and synthetic
  EDID.
- Explicit CRTC/encoder/plane modeset with a software vblank timer
  (60/120/144 Hz, lockdep-clean, deterministic pacing).
- Cursor plane and `FB_DAMAGE_CLIPS` damage tracking.
- Render node for masterless, zero-copy frame consumption; all ioctls are
  `DRM_RENDER_ALLOW`.
- DMA-BUF + sync_file export of the tracked scanout framebuffer.
- Owner-fd session lifecycle, stable output identity, strict atomic check.
- DRM ioctl UAPI (version/identity/caps/status/set-output/acquire/wait/metrics)
  and debugfs telemetry.
- End-to-end zero-copy validated on VAAPI (XRGB8888, linear).
- DKMS + Arch/CachyOS packaging.
- UAPI v9 multi-device prototype with independent DRM-master domains and a
  disposable two-device VM regression.
- Packaged private seat-broker instances and a VM regression with two
  unprivileged Weston DRM compositors scanning out concurrently on separate
  Hermes cards.

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
- Compositor recovery handling beyond owner-fd disconnect and hotplug.

## Out of scope (for now)

- A real DRM writeback connector. Doing this properly needs writeback-connector
  plumbing, not placeholder flags.
- Managing EVDI. EVDI stays a parallel fallback in Hermes, not something
  Hermes-KMS wraps.

See [driver-design.md](driver-design.md) for the architecture.
