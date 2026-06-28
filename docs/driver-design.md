# Hermes-KMS driver design

Hermes-KMS is a Linux DRM/KMS virtual display driver that replaces EVDI for
Hermes. It does not manage EVDI; EVDI remains a supported fallback.

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

## Modeset model

The CRTC, encoder, and primary plane are initialized explicitly (mirroring
vkms) rather than through `drm_simple_display_pipe`. This is what lets the
driver run a software vblank timer: an hrtimer fires `drm_crtc_handle_vblank()`
at the active mode's refresh, so the compositor composes the virtual output at
its full rate (60/120/144 Hz) and page-flip events are paced to the vblank.

A cursor plane lets the compositor offload pointer motion without recompositing
the whole output, and `FB_DAMAGE_CLIPS` on the primary plane lets the driver
forward the changed region to the capture consumer.

## Communication with Hermes

Hermes-KMS should be controlled through explicit DRM ioctls, not by scraping logs or guessing connector names.

The first UAPI lives in `include/uapi/drm/hermes_kms_drm.h` and provides:

- version discovery;
- stable output identity discovery;
- capability discovery;
- current output status;
- requested output enable/disable and preferred mode.

This is deliberately small. It can be safely extended with append-only structs and new ioctls for:

- frame queue descriptors;
- DMA-BUF export/acquire;
- explicit sync fences;
- latency counters;
- a real DRM writeback connector.

The stable Hermes output name is `HERMES-1`. The DRM connector name can still be
the core-generated `Virtual-*`; Hermes should use `GET_IDENTITY` and object IDs
instead of scraping connector names.

## Frame and scanout tracking

The driver tracks the current scanout framebuffer on every enable/update/flush callback and exposes the metadata through `GET_STATUS` and `ACQUIRE_FRAME`.

Tracked fields include:

- monotonically increasing frame sequence;
- update/enable/disable timestamps from `ktime_get_ns()`;
- framebuffer object ID;
- framebuffer size;
- DRM fourcc format;
- modifier;
- per-plane pitch and offset.

The same tracked framebuffer is refcounted and used for DMA-BUF export, sync_file
export, latest-frame waits, and status/debug metadata.

## Frame acquire contract

`DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME` is the initial userspace-facing frame contract.

Current behavior:

- without flags, returns the latest tracked frame metadata;
- returns `-ENODATA` if no frame has been tracked yet;
- exports DMA-BUF fds for the tracked framebuffer if userspace requests `HERMES_KMS_FRAME_REQUEST_DMABUF`;
- sets `HERMES_KMS_FRAME_DMABUF_VALID` only after all requested plane fds were exported successfully;
- returns the merged damage rectangle (`damage_x1..y2`, flagged by `HERMES_KMS_FRAME_DAMAGE_VALID`) when the compositor supplied `FB_DAMAGE_CLIPS`, so the consumer can encode only the dirty region.

Future behavior:

- preserve pitch, offset, modifier, format, and sequence metadata;
- validate/import the exported fds through the Hermes encoder path.

This avoids a compatibility trap: Hermes can start using one ioctl shape now, while the driver remains honest that DMA-BUF export is only the first stage. Real zero-copy still depends on whether the encoder can import the exported buffer format/modifier without an implicit copy.

`HERMES_KMS_FRAME_REQUEST_SYNC_FILE` returns a sync_file fd for the tracked
frame. Hermes-KMS creates this as a signaled frame-availability fence after the
atomic update path has accepted the framebuffer. It is correct for coordinating
Hermes consumption of the exported frame, but it is not a substitute for encoder
import validation.

## Low-latency frame wait

`DRM_IOCTL_HERMES_KMS_WAIT_FRAME` is the stream-side pacing primitive. Userspace
passes the last consumed `frame_sequence` and a timeout in milliseconds. The
driver sleeps on a waitqueue and wakes when the primary plane update path, pipe
enable path, or pipe disable path advances `frame_sequence`.

Expected Hermes loop:

```text
last_sequence = 0
while streaming:
    WAIT_FRAME(last_sequence, timeout_ms)
    ACQUIRE_FRAME(HERMES_KMS_FRAME_REQUEST_DMABUF |
                  HERMES_KMS_FRAME_REQUEST_SYNC_FILE)
    import the returned DMA-BUF into VAAPI/NVENC/AMF
    last_sequence = acquired.sequence
```

This avoids busy polling and gives Hermes a clean place to drop stale frames:
if multiple compositor commits happen before the encoder is ready, Hermes can
wait for the newest sequence and acquire only the latest framebuffer.

## Metrics

`DRM_IOCTL_HERMES_KMS_GET_METRICS` exposes counters and timestamps that Hermes
can sample during streaming:

- frame update count and current frame sequence;
- vblank count and vblank-overrun count (dropped page-flip slots);
- acquire count and no-frame acquire count;
- DMA-BUF and sync_file export success/failure counts;
- wait success, timeout, and interruption counts;
- output enable/disable and hotplug counts;
- owner-fd cleanup count;
- last update/acquire/wait/export timestamps.

These metrics are intentionally driver-local and monotonic for the module
lifetime. Hermes should use them for diagnostics and latency telemetry, not as a
stable persisted session log. The same counters are also readable as text at
`/sys/kernel/debug/dri/<n>/hermes_kms_stats`.

## Encoder Import Preflight

`tools/hermes-kms-import-check/hermes-kms-import-check` validates the next stage
after driver-side DMA-BUF export. It acquires a Hermes-KMS frame with DMA-BUF
and sync_file flags, then tries to import the DMA-BUF into VAAPI as a
`VASurface` using `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`.

This is intentionally narrower than a full stream encoder. It answers whether
the VA driver accepts the current scanout buffer format, pitch, modifier, and
DMA-BUF fd. If this fails for `XR24`/linear scanout, Hermes should treat that as
an integration decision point: request a different compositor format, add a GPU
conversion step to NV12/P010, or choose a backend-specific encoder path.

## Initial load configuration

The module supports simple load-time parameters for test safety:

- `initial_enabled`
- `initial_width`
- `initial_height`
- `initial_refresh_hz`

The default is `initial_enabled=0`, so the DRM device exists but the connector
starts disconnected. This prevents the desktop compositor from immediately
taking ownership of the virtual display during development or before Hermes has
started a stream.

`DRM_IOCTL_HERMES_KMS_SET_OUTPUT` connects the output and records the calling
`drm_file` as the session owner. Only that owner can update or disable the
output while it is active. If the owner fd closes, including process crash, the
driver marks the connector disconnected, clears the tracked frame, and emits a
hotplug event. `GET_STATUS` reports whether a session owns the output, plus the
current session ID and owner pid for diagnostics.

`SET_OUTPUT` is bidirectional. The request supplies the desired enable state and
mode; the response returns the applied mode, result flags, and session ID. This
lets Hermes start a stream with one ioctl, keep the returned session ID in its
stream state, and avoid an immediate status round-trip just to discover the
owner session.

For debug sessions, `tools/hermes-kmsctl/hermes-kmsctl hold 1920x1080@60`
enables the output and keeps the owner fd open until interrupted. The connector
starts disconnected; Hermes connects it when a stream starts, and the compositor
owns the real modeset path after the hotplug event.

The driver exposes a **render node** (`DRIVER_RENDER`), and all Hermes ioctls are
`DRM_RENDER_ALLOW`. Hermes opens the render node — never the primary node, which
would steal DRM master and EBUSY-block the compositor — so the compositor keeps
master and drives the modeset while Hermes pulls frames through the side channel.
`non_desktop` is a load-time parameter (default off) for cases where a compositor
should treat the output as non-desktop.

For isolated tests with `modetest`, the module can be loaded with
`initial_enabled=1 hotplug_events=0`. This creates a connected connector without
notifying KWin/GNOME/Xwayland, so the test tool can become DRM master and commit
a framebuffer directly.

Seat assignment is deliberately handled outside the kernel driver. Tags such as
`seat` and `master-of-seat`, plus `ID_SEAT` and `ID_AUTOSEAT`, are udev/logind
policy, not DRM driver state. Development and packaging install
`udev/99-hermes-kms-ignore-seat.rules` after the standard seat rules so local
test cards are not offered to the active desktop session until Hermes owns the
stream lifecycle. The same rule sets `GROUP="video"`, `MODE="0660"`, and
`TAG+="uaccess"` so Hermes can open the primary DRM node without running as
root when the local user has normal device access.

## Current status

Implemented and validated: explicit CRTC/encoder/plane modeset with a software
vblank timer (60/120/144 Hz, lockdep-clean, deterministic pacing), cursor plane,
damage tracking, render node for masterless zero-copy consumption, scanout
tracking, owner-fd lifecycle, stable output identity, DMA-BUF + sync_file export,
strict atomic check, and debugfs telemetry. End-to-end zero-copy is validated on
VAAPI (XRGB8888, linear).

Not yet implemented:

- a real DRM writeback connector;
- NVENC/AMF import validation (VAAPI is validated);
- NV12/P010 scanout and HDR (the compositor composes in RGB; the encoder does
  RGB→NV12 on the real GPU today);
- full compositor recovery handling beyond owner-fd disconnect and hotplug.
