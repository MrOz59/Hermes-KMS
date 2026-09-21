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
- Scanout modifier pass-through. Any tiled or compressed layout the
  compositor's render GPU produces is accepted, and `scanout_modifiers=`
  publishes the extra layouts an `IN_FORMATS`-driven compositor can negotiate.
  The driver never reads scanout pixels, so the layout intersection is
  userspace's to decide.
- Configurable mode envelope (`min_width`, `min_height`, `max_width`,
  `max_height`, `max_refresh_hz`) reported through `GET_CAPS` and reflected in
  the synthetic EDID's range limits, plus an optional reported physical panel
  size for compositors that derive a scale from it.
- Ten-bit scanout formats (`XRGB2101010`/`ARGB2101010`) alongside the eight-bit
  ones, with the advertised EDID depth selected by `color_depth=`.
- Runtime card creation and removal through configfs
  (`/sys/kernel/config/hermes-kms/`), matching the interface vkms adopted
  upstream. Cards no longer have to be drawn from a pool fixed at module load,
  and a project that only wants a virtual display can create one without
  writing any ioctl code.
- Per-card render-node ownership. A configfs-created card names the uid that
  owns it with `access_uid`, so several mutually untrusted consumers each hold
  their own card instead of sharing the one uid the packaged pool grants every
  Hermes render node.
- UAPI v13 session-capability lifecycle. `SESSION_ACCESS(ROTATE_TOKEN)` retires
  a token without interrupting the consumers already bound, and
  `REVOKE_BINDINGS` drops every binding at once while ownership, session ID and
  scanout survive. Before these, cutting off a leaked token meant disabling the
  output, which ends the stream. `hermes-kmsctl hold --control PATH` reaches
  both from the command line through a private FIFO, since the owner's
  authorization is the descriptor itself and a second invocation cannot have it.
- HDR *advertisement* behind `hdr_enable=1` (default off): a CTA-861 EDID
  extension carrying HDR Static Metadata (PQ) and BT2020 Colorimetry data
  blocks, plus the `HDR_OUTPUT_METADATA` and `Colorspace` connector properties,
  gated as one unit so the output never advertises a subset. This is what makes
  a compositor recognize the output as HDR-capable; it is not end-to-end HDR
  (see Next).
- Synthetic EDID pinned against a real parser, not only against its own test.
  `make check-edid-conformity` runs `edid-decode --check` over the generated
  bytes in all four configurations and compares the findings to a recorded
  baseline, so a new conformity failure surfaces as a diff instead of in
  someone else's parser.

- UAPI v14 frame-associated RGB colorspace and static HDR metadata capture,
  including metadata-only transitions and validation at atomic check time.

## Next

- Colour metadata in the capture UAPI, which is what HDR is still missing.
  `hdr_enable=1` gets a compositor to treat the output as HDR-capable, but a
  frame arrives at the consumer with no colorspace, EOTF or HDR metadata
  alongside it, so it cannot tell how to interpret those pixels. Advertisement
  is therefore in place and end-to-end HDR is not, including with
  `color_depth=10`, which is untested together with `hdr_enable=1`.
- Real-host validation of the two concurrency models: KWin adoption and
  persistence for `devices=1 outputs=N`, plus two simultaneous Moonlight
  sessions for `devices=N outputs=1`. Disposable VM tests already validate
  simultaneous owners/DRM masters, two concurrent Weston compositors, distinct
  framebuffer/DMA-BUF channels, independent disconnect, and clean unload.
- NVENC/AMF DMA-BUF import validation (VAAPI is validated).
- NV12/P010 scanout. The compositor composes in RGB and the encoder does
  RGB→NV12 on the real GPU today, so this is an optimization, not a blocker.
- Encoder consumption of the damage rectangle for partial-frame encode.
- Real-host compositor coverage beyond KWin. Weston (two concurrent DRM
  compositors) and GNOME/Mutter (cursor plane, imported scanout buffers) are
  covered by VM regressions; what is missing is the same on real hardware, and
  wlroots is untested either way.
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
