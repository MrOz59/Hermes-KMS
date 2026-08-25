# Hermes-KMS driver design

Hermes-KMS is a reusable Linux DRM/KMS virtual display driver. Hermes is its
reference consumer, but neither the capture interface nor its authorization
model depends on that application, Steam, or any process name. It does not
manage EVDI; EVDI remains a supported fallback for consumers that choose it.

## Target architecture

```text
Compositor / game
    ↓
HERMES-1..N DRM/KMS virtual connectors
    ↓
GPU-backed framebuffer / GEM object
    ↓
PRIME / DMA-BUF export
    ↓
Capture/streaming/recording consumer
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

### Scanout formats

The primary plane offers `XRGB8888`/`ARGB8888` and the four `2101010` variants.
The driver stores no pixels and reports the fourcc verbatim through
`ACQUIRE_FRAME`, so the list only has to cover what a compositor might compose
into and an encoder might import.

The ten-bit entries are the prerequisite for wide gamut and HDR, and are inert
by default: a compositor will not drive an output deeper than its sink claims to
accept, and the synthetic EDID advertises eight bits per primary unless
`color_depth=` says otherwise. Raising it is therefore a deliberate choice, and
consumers must be prepared for a ten-bit fourcc when it is made.

### Scanout layouts

The driver never samples a scanout pixel: it latches the framebuffer, holds a
reference, and re-exports the same buffer to the capture consumer, reporting
the modifier verbatim through `ACQUIRE_FRAME`. Any layout the compositor's
render GPU can produce is therefore acceptable, so the primary plane implements
`format_mod_supported` and accepts every modifier. Without that hook the DRM
core falls back to the plane's modifier list, and a plane initialised without
one gets the core default of `DRM_FORMAT_MOD_LINEAR` alone — which rejects a
tiled or compressed scanout in `drm_atomic_plane_check()` and makes the
compositor render into a detiled target for nothing.

Accepting a layout and advertising it are separate questions. A compositor that
allocates strictly from the plane's `IN_FORMATS` blob can only choose what that
blob lists, and the kernel cannot know which tiled or compressed layouts the
compositor's render GPU and the consumer's encoder both understand — that
intersection is a userspace property. `scanout_modifiers=` therefore lets
userspace, which can query both through GBM/EGL/VA, publish up to 15 extra
layouts alongside the always-present linear entry. The default is linear-only.

The cursor plane keeps the core's linear-only list and no pass-through hook.
Its image is published as a separate ARGB8888 stream that consumers are
documented to composite themselves, often on the CPU, so a tiled cursor would
move detiling into every consumer to save the compositor nothing measurable.

DRM's `mode_config.min_width/min_height` limit every framebuffer, including a
cursor, rather than only connector modes. Hermes therefore advertises a 1x1
framebuffer minimum while separately enforcing its configured minimum display
mode in mode validation and `SET_OUTPUT`; the cursor plane itself remains capped
at 256x256.

### Mode envelope

A virtual display has no panel to constrain it, so the accepted mode range is
policy rather than hardware: a streaming host wants whatever geometry its remote
client asked for. `min_width`, `min_height`, `max_width`, `max_height` and
`max_refresh_hz` configure that envelope, defaulting to 640x480 through
7680x4320 at up to 240 Hz, and are clamped into a 64..16384 pixel, 1..1000 Hz
hard limit at module load. `GET_CAPS` reports the configured values, so a
consumer never has to assume the defaults.

The synthetic EDID's display range limits descriptor is derived from the same
envelope. Those limits are not a hint — userspace validates modes against them
and the kernel infers extra DMT modes through `mode_in_range()` — so a
descriptor narrower than the driver's own envelope silently rules out modes the
connector advertises. The block is EDID 1.4 rather than 1.3 for three reasons:
1.3 cannot state more than 255 kHz of horizontal rate, while 1.4's byte-4 offset
flags double that ceiling; `drm_get_monitor_range()` ignores the descriptor
entirely on anything older; and it only publishes the range at all when the
feature byte declares a continuous-frequency display, which is exactly what this
driver is. The formula byte stays at "range limits only" for the same reason —
the kernel deliberately refuses to derive a monitor range from a GTF or CVT
descriptor.

An envelope too wide to state even in 1.4 (8K at 240 Hz needs roughly 1.2 MHz of
horizontal rate against a 510 kHz ceiling) saturates the descriptor and logs a
warning, so the resulting mode filtering is diagnosable instead of mysterious.

`physical_width_mm`/`physical_height_mm` optionally publish a panel size, which
is what a compositor divides into the mode to pick a scale factor. Both the
base block's centimetre fields and the detailed timing's millimetre fields are
written together, since userspace reads either. The default leaves the size
undefined.

`tests/edid.c`, wired into `make check`, builds the same bytes the module does
and decodes them the way `drivers/gpu/drm/drm_edid.c` decodes them.

## Multi-output prototype

UAPI v8 can expose 1–8 independent virtual outputs on a single DRM device
(`outputs=`, default 1 for compatibility). Multiple outputs are opt-in until
the consuming host integration can manage every connector. Every output owns a
separate connector, encoder, CRTC, primary/cursor planes, software-vblank
timer, owner session, scanout reference, frame waitqueue, metrics, and DMA-BUF
cache. This keeps one compositor-facing DRM card while preventing two capture
sessions from reading the same output state.

Output selection and authorization are scoped to a DRM file descriptor:

1. open the Hermes-KMS render node for a prospective owner;
2. call `DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT` with an available 0-based output
   index, then claim it with `SET_OUTPUT`;
3. use that owner fd directly, or obtain its session capability for a separate
   capture fd;
4. on a separate capture fd, call `SESSION_ACCESS(BIND)` with the output index,
   session ID and token. This selects and authorizes the active output
   atomically; do not precede it with `SELECT_OUTPUT`;
5. use the authorized fd for `WAIT_FRAME`, `ACQUIRE_FRAME`, `WAIT_UPDATE`,
   `ACQUIRE_CURSOR`, status and metrics, with one fd for every simultaneous
   output.

A newly opened fd defaults to output 0, preserving UAPI v7 output-selection
behavior for older clients. UAPI v11 authorization is still required for
protected operations. `SELECT_OUTPUT` cannot move a foreign fd onto an active,
owned output; `SESSION_ACCESS(BIND)` is the atomic authorized path for that
case. Rebinding an fd that owns an enabled output is also rejected. The friendly
identities are `HERMES-1`, `HERMES-2`, and so on; each connector has a distinct
EDID serial for compositor layout persistence. Compatibility was smoke-tested
with the unmodified v0.1.2 control client against the v8 module.

Status: **Prototype**. A disposable virtme-ng test validates simultaneous
atomic modesets, distinct owners/framebuffers, DMA-BUF + sync_file export,
independent disconnect, and clean unload for two outputs. KWin adoption,
persistent host layout, and real encoder integration still require validation.

## Multi-device session prototype

UAPI v9 can create 1–8 separate DRM devices with `devices=` (default 1). Unlike
`outputs=`, which places several connectors under one DRM-master domain,
separate devices allow separate compositors to hold DRM master concurrently.
Each device owns its complete set of KMS objects, output/session state, frame
queues, and platform-device lifetime.

The identity ioctl reports a stable 0-based `device_index` and total
`device_count`. Friendly output names and EDID serials remain globally unique
across devices. The intended isolated-session layout is `devices=N outputs=1`;
the older shared-desktop layout remains `devices=1 outputs=N`.
For compatibility, `devices=1` retains the original `hermes-kms` platform path
and host seat. Multi-device paths use `hermes-kms.0..7`, which the packaged
udev rule maps to stable `hermes-kms-1..8` seats for matching compositor and
virtual-input isolation.

Status: **Prototype**. `scripts/vm-multi-device-test.sh` validates two
simultaneous DRM masters at different modes, distinct owners/framebuffers,
DMA-BUF + sync_file export from both devices, independent disconnect, and clean
module unload. `scripts/vm-multi-compositor-test.sh` additionally validates two
concurrent Weston DRM sessions, each connected through its own packaged
`hermes-kms-seatd@.service`-compatible private broker, with distinct scanout
modes. The private brokers isolate seatd's single-active-client state; they are
not a security boundary between mutually untrusted local users. Encoder import,
input seats, and two Moonlight clients are not validated by these driver-only
tests.

### Host-compatible automatic pool

UAPI v10 adds `session_devices=N`. Unlike the legacy `devices=N` topology, it
creates one host-role card plus N session-role cards. The host card remains on
seat0 for KWin/GNOME and normal shared-desktop streaming. Each session card
reports a stable 1-based `session_index`; udev maps that index to
`hermes-kms-N` and requests the matching private seat broker.
Session-role primary nodes and unconfigured render nodes are root-only mode
0600. The root broker opens the primary node and passes access over its
per-session socket; global `video` membership and active-seat `uaccess` ACLs do
not bypass that boundary. The setup helper grants only the configured consumer
UID access to render nodes for capability-protected control and capture.

The role and session metadata replace reserved identity words, so the ioctl
struct size and all older fields remain unchanged. The new
`HERMES_KMS_CAP_SESSION_DEVICE_POOL` capability gates interpretation of those
fields. Old UAPI clients continue to see globally unique cards and outputs.

The packaged default is `session_devices=4 outputs=1 initial_enabled=0`.
Disconnected cards have KMS object state but no active scanout framebuffer, so
the pool does not preallocate four display-sized buffers. A setup helper stores
the configured service uid for broker socket ownership. If the loaded topology
does not match, it requests a reboot instead of forcibly unloading a card held
by a compositor. The packaged polkit action gives the authenticated setup
request a Hermes-specific prompt and permits it only from an active local
session. The helper also migrates legacy manually-enabled broker units to
udev-managed activation and stops stale instances outside the selected pool.

Status: **Prototype**. A disposable VM regression validates one host plus two
private cards, role/index identity, udev seat mapping, and systemd broker wants.
Another regression exercises the setup helper's persistent configuration,
broker restart scope, and reboot-required migration. The existing
two-compositor regression validates direct configured-user socket ownership
without requiring `seat` group membership.

## Runtime card creation

Every topology above is chosen at module load, which means a host can only draw
from a pool decided in advance, and changing that pool means reloading a module
a compositor is holding. configfs removes that constraint, and is the interface
vkms adopted upstream for the same problem:

```bash
mkdir  /sys/kernel/config/hermes-kms/stream-1
echo 1 > /sys/kernel/config/hermes-kms/stream-1/outputs
echo session > /sys/kernel/config/hermes-kms/stream-1/role
echo 3 > /sys/kernel/config/hermes-kms/stream-1/session_index
echo 1 > /sys/kernel/config/hermes-kms/stream-1/enabled
cat    /sys/kernel/config/hermes-kms/stream-1/card         # cardN
cat    /sys/kernel/config/hermes-kms/stream-1/render_node  # renderDN
rmdir  /sys/kernel/config/hermes-kms/stream-1
```

`outputs`, `role` and `session_index` are writable only while the card is
disabled, because the KMS object graph is built once at probe; writing them on a
live card returns `EBUSY`. Everything an identity needs is therefore settled
before the card exists, so a dynamically created session card lands on the same
`hermes-kms-N` seat and private broker a pool card would. A `session` role with
index 0 is refused, since it would match no seat rule. `card` and `render_node`
report the nodes the card received, letting a controller find what it just
created without racing udev. `rmdir` on a live card removes it, as a hot-unplug
would.

This also makes the driver usable without writing any ioctl code: a project that
only wants a virtual display and already captures through its own pipeline can
create one with `mkdir` and a couple of writes.

Two consequences follow for consumers, and `HERMES_KMS_CAP_DYNAMIC_DEVICES`
announces them. `GET_IDENTITY.device_count` is the number of cards that exist
right now rather than a load-time constant, and `device_index` values are
neither dense nor stable across a card being removed and recreated. Identify a
card by its role and session index, or by the node the creating controller
recorded, rather than by walking indices.

Output names and EDID serials are allocated from a module-wide pool rather than
derived from `device_index * output_count`, which could only stay unique while
every card had the same output count and existed from the start. Removal uses
`drm_dev_unplug()` rather than `drm_dev_unregister()`: a compositor can still
hold a card open when it goes, and unplug is what makes `drm_ioctl()` answer
`ENODEV` instead of running against a device being torn down.

The module holds a reference while any configfs group exists, so `rmmod` fails
until the groups are removed. Statically configured cards and the session pool
are unaffected and keep working exactly as before.

Status: **Prototype**. `scripts/vm-configfs-test.sh` validates creation and
removal alongside a static card, role and seat metadata reaching sysfs and
`GET_IDENTITY`, unique output naming across cards, rejected writes on a live
card, refused unload, repeated enable/disable cycles, `rmdir` on a live card,
and a three-card pool created and removed in one run.

## Public userspace UAPI

Hermes-KMS is controlled through explicit DRM ioctls, not by scraping logs or
guessing connector names. The UAPI is public and consumer-neutral.

The current UAPI is version 11 and lives in
`include/uapi/drm/hermes_kms_drm.h`. It provides:

- version discovery;
- stable output identity discovery;
- capability discovery;
- current output status;
- requested output enable/disable and preferred mode;
- generic session-capability handoff;
- frame wait/acquire with DMA-BUF and explicit synchronization;
- independently sequenced cursor wait/acquire with DMA-BUF and explicit
  synchronization;
- capture, output-lifecycle and vblank metrics. Visible width is
  preserved exactly even when it is not divisible by eight; framebuffer pitch
  alignment remains a separate allocation property.

The interface is deliberately small. Future extensions must use new ioctls or
reserved fields without moving existing members. All 64-bit UAPI values use
`__aligned_u64`, and v11 explicitly fills what used to be LP64-only padding in
`GET_STATUS`. `tests/uapi-abi.c` asserts sizes, alignment-sensitive offsets and
encoded ioctl values for native and ILP32 builds.

A consumer must first identify a candidate fd through the DRM core
`DRM_IOCTL_VERSION` ioctl (or libdrm's `drmGetVersion()`) and require the exact
name `hermes-kms`. Only then may it issue the driver-private ioctls. Private DRM
command numbers are scoped to the identified driver and can collide with a
different driver's command table, so a private `GET_VERSION` is not itself a
safe device probe.

The reuse boundary is also explicit: kernel/module code is GPL-2.0, the public
UAPI header is `GPL-2.0 WITH Linux-syscall-note`, and the optional header-only
session helper plus its userspace-only tests are MIT-licensed. The kernel ABI
does not import Hermes application policy, and an independent program can use
the documented ioctls or MIT helper without becoming a Hermes application.

For example, a real DRM writeback connector would require a new, explicit UAPI
extension; the driver does not advertise a placeholder capability for it.

The stable driver output names are `HERMES-1` through `HERMES-N`. DRM connector
names can still be core-generated `Virtual-*`; consumers should use
`GET_IDENTITY`, `GET_CAPS.output_count`, and object IDs instead of scraping
connector names. When `HERMES_KMS_CAP_CURSOR_CAPTURE` is advertised,
`GET_IDENTITY.cursor_plane_id` identifies the associated cursor plane.

### Generic session capability

UAPI v11 advertises `HERMES_KMS_CAP_SESSION_TOKEN`. An fd that successfully
claims an available output with `SET_OUTPUT` becomes that session's owner. The
owner sends `SESSION_ACCESS(GET_TOKEN)` with only `operation` set and all other
request fields zero; the response contains a random 128-bit opaque token, the
session ID, output index and `RESULT_TOKEN_VALID`. A different fd passes all
three values to `SESSION_ACCESS(BIND)`, which selects
and authorizes the active output atomically before that fd uses status, capture,
wait or metrics ioctls. Calling `SELECT_OUTPUT` first is unnecessary and is
rejected for an output owned by another session. A successful bind returns
`RESULT_BOUND` and clears the token fields in its response. `UNBIND` drops that
fd's access.

Version, capability and identity discovery remain public. `GET_STATUS` is also
public while an output is fully idle so a controller can decide whether to
claim it; once a session is active, status and all capture/metrics operations
require the owner or a valid binding. Revoking a session wakes blocked waiters,
which revalidate authorization and return an access error instead of consuming
state from a replacement session.

UAPI v13 adds the two operations that make a session's authorization
manageable rather than all-or-nothing. `ROTATE_TOKEN` replaces the token while
leaving the session and every existing binding intact, so a token that may have
been exposed stops granting new binds without interrupting the consumers already
running. `REVOKE_BINDINGS` additionally drops every binding at once: bound
descriptors fail their next protected ioctl with `EACCES`, blocked waits are
woken to the same error, and ownership, the session ID and the scanout all
survive. It rotates the token as part of the same operation, because a
revocation that left the old token usable would let the same holder simply bind
again. Both are owner-only and both return the new token. An owner blocked in
its own `WAIT_FRAME` on another thread sees that wait fail with `EACCES` across
a revocation and should reissue it.

Without these, the only way to cut off a consumer was to disable the output,
which ends the stream — so a leaked token effectively could not be answered.

`GET_STATUS.bound_fd_count` reports how many descriptors are bound to the live
session, excluding the owner, and `GET_METRICS` adds `bind_count`,
`bind_reject_count`, `unbind_count` and `binding_revoke_count`. A broker that
handed its capability to one worker and sees two is looking at a leak;
`bind_reject_count` is what a brute-force attempt would move.

The driver stores no executable name, Steam application ID, UID or TGID policy.
The owner can pass the capability through any application-defined trusted IPC
channel, including to a sandboxed or separately supervised worker. Closing the
owner fd or disabling the output invalidates the token and all bindings from
that session, preventing subsequent protected ioctls and subsequent fd exports.
Linux cannot recall a DMA-BUF fd that `ACQUIRE_FRAME` already installed: its
holder retains access to that buffer until the fd is closed. Revocation is
therefore about future access, and `GET_METRICS.cross_session_buffer_export_count`
reports the case where that matters most: a frame exported while the same buffer
object was simultaneously the scanout of another output owned by a different
session. A compositor mirroring one buffer onto two outputs does this
legitimately, so the driver counts it rather than refusing — but while it is
non-zero those two consumers are not isolated from each other. A design that gives
capture access to an untrusted process must therefore put a trusted broker/copy
boundary in front of it or guarantee per-session BO isolation and no reuse
across the trust boundary. Userspace must also treat the token like any other
credential: do not log it or expose it in process arguments or world-readable
storage. Device-node access remains a separate udev/logind policy decision.

The command-line tools provide `--session-file` as a diagnostic convenience:
`hermes-kmsctl hold` publishes a no-replace, mode-0600 file owned by the current
UID, and capture tools read, bind and promptly erase their in-memory copy. This
file format is not part of the kernel UAPI and is not the recommended transport
for applications; production integrations should normally keep the token in
memory and use their existing trusted IPC.

## Frame and scanout tracking

The driver latches the current primary scanout framebuffer and its metadata as
one coherent snapshot during atomic flush, and exposes it through `GET_STATUS`
and `ACQUIRE_FRAME`.

Tracked fields include:

- monotonically increasing primary-frame sequence (cursor-only commits do not
  advance it);
- update/enable/disable timestamps from `ktime_get_ns()`;
- framebuffer object ID;
- framebuffer size;
- DRM fourcc format;
- modifier;
- per-plane pitch and offset.

The same tracked framebuffer is refcounted and used for DMA-BUF export, sync_file
export, latest-frame waits, and status/debug metadata.

## Frame acquire contract

`DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME` is the userspace-facing frame contract.

Current behavior:

- without flags, returns the latest tracked frame metadata;
- returns `-ENODATA` if no frame has been tracked yet;
- exports DMA-BUF fds for the tracked framebuffer if userspace requests
  `HERMES_KMS_FRAME_REQUEST_DMABUF`;
- sets `HERMES_KMS_FRAME_DMABUF_VALID` only after all requested plane fds were
  exported successfully;
- returns the compositor's merged, half-open damage rectangle
  (`damage_x1..y2`, flagged by `HERMES_KMS_FRAME_DAMAGE_VALID`) for a consecutive
  new sequence. Re-acquiring the same sequence returns a valid empty rectangle,
  meaning no additional primary-plane change. On a first acquire, a sequence
  gap, or a commit without valid damage, userspace must process the whole frame.

Real zero-copy still depends on whether the chosen encoder can import the
exported format, modifier and layout without an implicit copy. That is a
userspace/backend property rather than an application-specific kernel path.

`HERMES_KMS_FRAME_REQUEST_SYNC_FILE` returns a sync_file fd for the tracked
framebuffer's implicit write fence from `dma_resv`. If there is no pending
writer, the returned fence is already signalled. The consumer must wait before
sampling the DMA-BUF; synchronization does not replace encoder import
validation.

## Low-latency frame wait

`DRM_IOCTL_HERMES_KMS_WAIT_FRAME` is the stream-side pacing primitive. Userspace
passes the last consumed `frame_sequence` and a timeout in milliseconds. The
driver sleeps on a waitqueue and wakes when the primary plane update path, pipe
enable path, or pipe disable path advances `frame_sequence`.

Expected capture loop (after `SESSION_ACCESS(BIND)`):

```text
last_sequence = 0
while streaming:
    WAIT_FRAME(last_sequence, timeout_ms)
    ACQUIRE_FRAME(HERMES_KMS_FRAME_REQUEST_DMABUF |
                  HERMES_KMS_FRAME_REQUEST_SYNC_FILE)
    import the returned DMA-BUF into VAAPI/NVENC/AMF
    last_sequence = acquired.sequence
```

This avoids busy polling and gives a consumer a clean place to drop stale
frames: if multiple compositor commits happen before the encoder is ready, it
can wait for the newest sequence and acquire only the latest framebuffer. A
sequence gap deliberately disables partial-damage validity for that acquire.

## Separate cursor stream

The primary framebuffer deliberately remains stable when a compositor performs
a cursor-plane-only commit. Consumers that need the hardware cursor check
`HERMES_KMS_CAP_CURSOR_CAPTURE` and use
`DRM_IOCTL_HERMES_KMS_WAIT_UPDATE` instead of relying only on `WAIT_FRAME`.
`WAIT_UPDATE` takes the last consumed primary and cursor sequences and reports
which stream advanced. It returns the current sequence numbers, timestamps and
output status from one coherent state-lock snapshot. The response uses
`HERMES_KMS_WAIT_UPDATE_FRAME_READY` and
`HERMES_KMS_WAIT_UPDATE_CURSOR_READY`; a zero-timeout poll with no update
returns `-EAGAIN`, a blocking timeout returns `-ETIMEDOUT`, and revocation takes
precedence as `-EACCES`.

Primary `frame_sequence` and cursor `cursor_sequence` are nevertheless
independent streams, not the halves of a transaction ID. A coherent
`WAIT_UPDATE` response does not promise that the two values originated in the
same atomic compositor commit. Both may advance before userspace runs, and a
subsequent latest-state acquire may already see a newer value. Consumers retain
the newest complete state for each stream, interpret each READY bit relative to
its matching input sequence, and tolerate gaps.

`DRM_IOCTL_HERMES_KMS_ACQUIRE_CURSOR` returns a coherent cursor-state snapshot.
Without request flags it is metadata-only;
`HERMES_KMS_CURSOR_REQUEST_DMABUF` and
`HERMES_KMS_CURSOR_REQUEST_SYNC_FILE` request the current image fds and its
implicit write fence. Consumers must wait on that fence before sampling the
image. The cursor plane is `DRM_FORMAT_ARGB8888` with premultiplied RGB and
per-pixel alpha at full plane opacity. Software composition therefore uses
premultiplied-alpha blending and must not multiply the source RGB by alpha
again.

The geometry distinguishes the nominal image from its on-screen clipping:

- `position_x/y` is the unclipped top-left of the full cursor image in CRTC
  coordinates;
- when `HOTSPOT_VALID` is set, `hotspot_x/y` is relative to that full image,
  making the logical pointer location `position + hotspot`; Hermes uses a
  universal cursor plane, so ordinary compositors instead pre-adjust
  `position_x/y` and leave this optional validity bit clear;
- `crtc_x/y/w/h` is the clipped integer destination rectangle;
- `src_x/y/w/h` is the corresponding clipped source rectangle in DRM 16.16
  fixed-point pixels.

The consumer uses the validity flags before reading each metadata group and
composes only the clipped rectangles. `VISIBLE` is separate from
`BUFFER_VALID`, so a hidden or fully clipped cursor can keep a reusable image.
`cursor_sequence` covers position, visibility and image-state updates;
`image_sequence` advances conservatively whenever the current buffer may need
to be sampled again, including possible in-place pixel changes.

Framebuffer replacement can race the work required to install DMA-BUF or
sync_file fds. In that case `ACQUIRE_FRAME` or `ACQUIRE_CURSOR` returns
`-ESTALE` instead of mixing stale metadata with a new fd. A consumer discards
that attempt and retries the latest-state acquire with a small bounded loop.
The interface is a latest-state channel, not a queue, so skipped intermediate
cursor positions are expected when the consumer is slower than the compositor.

## Metrics

`DRM_IOCTL_HERMES_KMS_GET_METRICS` exposes counters and timestamps that an
authorized consumer can sample during streaming:

- frame update count and current frame sequence;
- vblank callback count and vblank-overrun count (timer intervals missed when
  a callback runs late, not rejected capture frames);
- acquire count and no-frame acquire count;
- DMA-BUF and sync_file export success/failure counts;
- wait success, timeout, and interruption counts;
- output enable/disable and hotplug counts;
- owner-fd cleanup count;
- last update/acquire/wait/export timestamps.

UAPI v11 assigns the two vblank counters from previously reserved words, so the
metrics struct size is unchanged. Metrics are intentionally driver-local and
monotonic for the module lifetime. Consumers should use them for diagnostics
and latency telemetry, not as a stable persisted session log. The same counters
are also readable as text at
`/sys/kernel/debug/dri/<n>/hermes_kms_stats`.

## Encoder Import Preflight

`tools/hermes-kms-import-check/hermes-kms-import-check` validates the next stage
after driver-side DMA-BUF export. It acquires a Hermes-KMS frame with DMA-BUF
and sync_file flags, then tries to import the DMA-BUF into VAAPI as a
`VASurface` using `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`.

This is intentionally narrower than a full stream encoder. It answers whether
the VA driver accepts the current scanout buffer format, pitch, modifier, and
DMA-BUF fd. If this fails for `XR24`/linear scanout, the consumer should treat
that as an integration decision point: request a different compositor format,
add a GPU conversion step to NV12/P010, or choose a backend-specific encoder
path.

## Initial load configuration

The module supports these topology and initial-state parameters:

- `initial_enabled`
- `initial_width`
- `initial_height`
- `initial_refresh_hz`
- `min_width`, `min_height`, `max_width`, `max_height`, `max_refresh_hz`
- `physical_width_mm`, `physical_height_mm`
- `color_depth`
- `outputs` (1–8, default 1, fixed until the module is reloaded)
- `devices` (1–8, legacy multi-device topology)
- `session_devices` (0 disables the pool; 1–8 creates that many private cards
  plus a host card, mutually exclusive with an explicit `devices` topology)
- `hotplug_events`
- `non_desktop`
- `scanout_modifiers` (up to 15 extra `IN_FORMATS` layouts; linear is always
  advertised and every modifier is accepted regardless of this list)

Topology, initial mode/state and `non_desktop` are read-only after module load.
`hotplug_events` remains a runtime diagnostic switch. The root-only
`insecure_legacy_unbound_access=1` escape hatch restores pre-v11 unbound
status/capture/wait/metrics access for old diagnostic clients; it weakens local
capture isolation and must not be enabled in a normal installation. It is
load-time only and logs a warning when set, because widening every output's
capture access on a live system, silently and mid-session, is not something an
escape hatch should be able to do.

The default is `initial_enabled=0`, so the DRM device exists but the connector
starts disconnected. This prevents the desktop compositor from immediately
taking ownership of the virtual display during development or before a
controller has started a stream.

`DRM_IOCTL_HERMES_KMS_SET_OUTPUT` connects the fd-selected output and records
the calling `drm_file` as the session owner. Only that owner can update or
disable the output while it is active. If the owner fd closes, including
process crash, the driver marks the connector disconnected, clears the tracked
frame, and emits a hotplug event. `GET_STATUS` reports whether a session owns
the output, plus the current session ID and owner pid for diagnostics.

`SET_OUTPUT` is bidirectional. The request supplies the desired enable state and
mode; the response returns the applied mode, result flags, and session ID. This
lets any controller start a stream with one ioctl, keep the returned session ID
in its state, and avoid an immediate status round-trip just to discover the
owner session. Under UAPI v11 the owner then retrieves the generic session token
for any separate capture fd that it chooses to authorize.

Real output changes are limited to a burst of ten per output per second to
bound hotplug/reprobe work from a faulty controller. Exact idempotent requests
and an already-idle disable do not consume the limit. A controller that receives
`EAGAIN` should retry with bounded backoff; no state was changed by the rejected
request.

For debug sessions, `tools/hermes-kmsctl/hermes-kmsctl hold 1920x1080@60`
enables the output and keeps the owner fd open until interrupted. The connector
starts disconnected; a controller connects it when a stream starts, and the
compositor owns the real modeset path after the hotplug event.

The driver exposes a **render node** (`DRIVER_RENDER`), and all private ioctls
are `DRM_RENDER_ALLOW`. A controller/capture consumer opens the render node —
never the primary node, which would steal DRM master and EBUSY-block the
compositor — so the compositor keeps master and drives the modeset while the
consumer pulls frames through the side channel.
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
test cards are not offered to the active desktop session until a test controller
owns the stream lifecycle. The rule controls local device-node permissions so
tests can run without making application identity part of the kernel UAPI.

## Current status

Implemented and validated for the original single-output path: explicit
CRTC/encoder/plane modeset with a software vblank timer (60/120/144 Hz,
lockdep-clean, deterministic pacing), cursor plane, damage tracking, render node
for masterless zero-copy consumption, scanout tracking, owner-fd lifecycle,
stable output identity, DMA-BUF + sync_file export, strict atomic check, and
debugfs telemetry. End-to-end zero-copy is validated on VAAPI (XRGB8888,
linear). Generic UAPI v11 session-capability binding, separate cursor capture,
and native/ILP32 ABI checks are also implemented.

Implemented and VM-validated as a prototype: multiple independent KMS
pipelines on one DRM device and per-fd output selection. This is not yet
validated with KWin and real encoders on the host.

Not yet implemented:

- a real DRM writeback connector;
- NVENC/AMF import validation (VAAPI is validated);
- NV12/P010 scanout and HDR (the compositor composes in RGB; the encoder does
  RGB→NV12 on the real GPU today);
- full compositor recovery handling beyond owner-fd disconnect and hotplug.
