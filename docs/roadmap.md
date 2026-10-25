# Hermes-KMS roadmap

This roadmap describes `main` (driver 0.4.0, UAPI v14), reviewed on 2026-10-25.
Implemented driver features, VM coverage and end-to-end product validation are
separate milestones. See [the changelog](../CHANGELOG.md) for release history.

## Implemented

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
- Per-card render-node access. A configfs-created card names the uid that
  receives a read-only ACL with `access_uid`, so separate consumers can each
  hold their own card instead of sharing the one uid the packaged pool grants every
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
  a compositor recognize the output as HDR-capable; activation is still the
  compositor's decision.
- Synthetic EDID pinned against a real parser, not only against its own test.
  `make check-edid-conformity` runs `edid-decode --check` over the generated
  bytes in eight configurations and compares the findings to a recorded
  baseline when parser versions match. Every version must report conformity
  PASS with no failures; decoded HDR blocks/checksums are also always checked.
- UAPI v14 frame-associated colour and static HDR10 metadata through
  `ACQUIRE_FRAME2`, including colour-only sequence updates and unchanged legacy
  ioctl layouts. Driver/VM tests cover atomic metadata delivery, authorization,
  malformed state and nonblocking transitions. Full GPU/client HDR validation
  remains an integration task; see [hdr-capture.md](hdr-capture.md).
- Generic runtime display profiles: per-card modes, physical size, HDR,
  modifiers, custom EDID identity/output prefix and stable serial selection.
  `devices=0` supports runtime-only use. See [configfs.md](configfs.md).
- Read-only capture ACLs so Chromium/Electron do not select the capture-only
  render node as a renderer, while active-seat/private users retain capture.
- VT-free private seat brokers (`SEATD_VTBOUND=0`), preserving the host VT.
- Header-based compatibility selection for DRM atomic/vblank/shmem/platform
  helper differences; CI builds Arch rolling, Ubuntu LTS and Fedora kernels.

## Next: integration and hardware validation

- Real-host validation of the two concurrency models: KWin adoption and
  persistence for `devices=1 outputs=N`, plus two simultaneous Moonlight
  sessions for `devices=N outputs=1`. Disposable VM tests already validate
  simultaneous owners/DRM masters, two concurrent Weston compositors, distinct
  framebuffer/DMA-BUF channels, independent disconnect, and clean unload.
- Correct full-frame direct NVIDIA DMA-BUF import and AMF import validation.
  Current Hermes NVENC uses a CPU scanout copy with hardware conversion/encoding;
  that is separate from direct import or zero-copy. Keep that fallback until
  pixel correctness, synchronization, reuse and teardown pass on real NVIDIA
  hardware.
- NV12/P010 scanout. The compositor composes in RGB and the encoder does
  RGB→NV12 on the real GPU today, so this is an optimization, not a blocker.
- Encoder consumption of the damage rectangle for partial-frame encode.
- Real-host compositor coverage beyond KWin. Weston (two concurrent DRM
  compositors) and GNOME/Mutter (cursor plane, imported scanout buffers) are
  covered by VM regressions; what is missing is the same on real hardware, and
  `scripts/vm-wlroots-test.sh` exercises labwc's DRM backend with pixman in a
  VM. GPU rendering and Hyprland/Aquamarine need their own validation.
- Real-host validation of separate cursor composition, including clipped cursor
  edges, hotspot placement, visibility transitions, and in-place image updates.
- Compositor recovery handling beyond owner-fd disconnect and hotplug.

## Separate experiments

The [`experimental/nvidia-zero-copy`](https://github.com/MrOz59/Hermes-KMS/tree/experimental/nvidia-zero-copy)
branch adds GPU-origin scanout/passthrough probes, full-frame EGL/CUDA content
checks, modifier inspection and cross-process import-cache coherence experiments.
These tools do not establish that KWin allocates the same layout or that a full
NVENC streaming path is correct. They are not part of `main`.

`main` contains the opt-in `huge_gem` allocation experiment and system-memory
import diagnostics. Huge-page allocation depends on kernel support and available
memory; it is not a proven NVIDIA fix. The historical import finding and
reproduction steps are in the [EGL checker guide](../tools/hermes-egl-import-check/README.md).

## Out of scope (for now)

- A real DRM writeback connector. Doing this properly needs writeback-connector
  plumbing, not placeholder flags.
- Managing EVDI. EVDI stays a parallel fallback in Hermes, not something
  Hermes-KMS wraps.
- Deciding which application or sandbox should receive a session token. That is
  userspace policy transported over the consuming project's trusted IPC.

See [driver-design.md](driver-design.md) for the architecture.
