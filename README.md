# Hermes-KMS

Hermes-KMS is a Linux DRM/KMS virtual display driver.

It is an EVDI alternative: a virtual display backend that streams the
compositor's scanout straight into a hardware encoder as a DMA-BUF, with no CPU
readback. Hermes is the reference consumer, but the driver is not tied to it —
any program that speaks its DRM ioctl UAPI can consume frames (see
[Using it in other projects](#using-it-in-other-projects)).

## What this is

- A Linux kernel DRM/KMS driver (`hermes_kms.ko`).
- An EVDI alternative for low-latency screen capture, used by Hermes today.
- A virtual display that exposes an output (`HERMES-1`) which a desktop
  compositor (KWin/GNOME) drives like a normal monitor.
- A zero-copy DMA-BUF source for VAAPI/other hardware encoders.

## What this is not

- It is not an EVDI manager or a userspace wrapper around EVDI.
- It is not a render GPU: it exports the compositor's framebuffer; the encode
  still runs on a real GPU that imports the exported DMA-BUF.

## How it works

The compositor owns the card and the capture consumer only reads frames:

1. The driver exposes a **render node** (`DRIVER_RENDER`). Without it, compositors
   skip the GPU and never enumerate the virtual connector.
2. The compositor (KWin/GNOME) takes DRM master on the primary node, enables
   the `HERMES-1` connector, and scans out the desktop into its framebuffer.
3. Hermes opens the **render node** (never the primary node, which would steal
   DRM master and EBUSY-block the compositor) and pulls the current scanout as
   DMA-BUFs via `ACQUIRE_FRAME` — all Hermes ioctls are `DRM_RENDER_ALLOW`.
4. A real GPU imports those DMA-BUFs and encodes them. The frame never leaves
   the GPU.

Measured capture cost on KWin at 720p: ~8 us/frame (`ACQUIRE_FRAME`) versus
~180 us/frame for EVDI's CPU copy, and constant regardless of resolution.

## Using it in other projects

The driver is not Hermes-specific. "Hermes" here is the reference consumer, not
a requirement: there is no per-process gating, no exported Hermes-only symbol,
and the capture path is a plain DRM ioctl UAPI over the render node. Any
userspace consumer — another streaming host such as Apollo/Sunshine, a recorder,
or your own tool — can use it by talking the same UAPI, with no fork required:

1. Open the render node and probe with `GET_VERSION` / `GET_CAPS`.
2. Optionally `SET_OUTPUT` to request a specific mode (this does not take DRM
   master, so the compositor keeps it).
3. Run the `WAIT_FRAME` → `ACQUIRE_FRAME` loop and import the returned DMA-BUF
   into your encoder.

[tools/hermes-kmsctl](tools/hermes-kmsctl) and
[tools/hermes-kms-import-check](tools/hermes-kms-import-check) are small,
self-contained reference consumers you can read or copy. The UAPI is documented
in [include/uapi/drm/hermes_kms_drm.h](include/uapi/drm/hermes_kms_drm.h) and in
[Userspace communication](#userspace-communication) below.

What is currently fixed (and would need a fork to rebrand, though not to use):
the output name is `HERMES-1` and the UAPI symbols are prefixed `HERMES_KMS_`.

What is currently validated: VAAPI with `XRGB8888`, linear. NVENC/AMF and
NV12/P010/HDR are not validated yet — see the [Roadmap](#roadmap). Forks and
contributions extending those paths are welcome under the project's license.

The project is GPL-2.0 licensed, so forks are free and must stay open under the
same terms — see [License](#license).

## Features

- out-of-tree kernel module: `hermes_kms.ko`;
- explicit CRTC/encoder/plane modeset with a software vblank timer, so the
  compositor composes the virtual output at its full refresh (60/120/144 Hz);
- cursor plane so the compositor offloads pointer motion;
- damage tracking (`FB_DAMAGE_CLIPS`) forwarded to the capture consumer via
  `ACQUIRE_FRAME`;
- render node for masterless, zero-copy frame consumption;
- synthetic EDID so compositors treat `HERMES-1` as a normal monitor;
- exact requested mode synthesized via CVT and re-probed on `SET_OUTPUT`, so
  arbitrary client geometries (e.g. 1280x720@30) modeset correctly;
- DMA-BUF export of the tracked scanout framebuffer, cached per buffer object;
- real `dma_resv` write fence exported as a sync_file;
- Hermes/apps UAPI through DRM ioctls; frame/metric tracking;
- debugfs telemetry at `/sys/kernel/debug/dri/<n>/hermes_kms_stats`;
- debug/control tool: `tools/hermes-kmsctl/hermes-kmsctl`;
- 640x480 through 3840x2160 mode range, 1920x1080 preferred.

## Build

Install matching kernel headers first.

On CachyOS/Arch-like systems:

```bash
make
```

Clean:

```bash
make clean
```

## Local test commands

Loading an unsigned experimental kernel module can destabilize the session. Use
this only on a test machine. Two ways to drive the output are described below:
compositor-driven (real streaming) and isolated `modetest` (driver validation).

Inspect the driver with the control tool while it is loaded:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1
sleep 1
tools/hermes-kmsctl/hermes-kmsctl version
tools/hermes-kmsctl/hermes-kmsctl identity
tools/hermes-kmsctl/hermes-kmsctl caps
tools/hermes-kmsctl/hermes-kmsctl status
tools/hermes-kmsctl/hermes-kmsctl metrics
tools/hermes-kmsctl/hermes-kmsctl wait 0 1000
tools/hermes-kmsctl/hermes-kmsctl --verbose status
tools/hermes-kmsctl/hermes-kmsctl hold 1920x1080@60
ls -l /dev/dri/
modetest -c
drm_info
journalctl -k -g hermes-kms
sudo rmmod hermes_kms
```

Optional initial mode/state parameters:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_width=1920 initial_height=1080 initial_refresh_hz=60
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1
```

There are two ways to drive the output, for two different purposes.

### Compositor-driven (real streaming)

This is the path Hermes uses. Load the module enabled and let the desktop
compositor adopt the connector:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1
```

The card must stay on the active logind seat so the compositor opens it — do
**not** install the dev seat-ignore rule for this. KWin/GNOME enable `HERMES-1`,
commit a framebuffer, and Hermes pulls frames from the render node. A userspace
owner may still call `SET_OUTPUT` (via the render node) to request the client's
exact mode; that owner does not take DRM master, so the compositor keeps it.

### Isolated `modetest` (driver validation, no compositor)

To exercise the driver without a compositor, load it connected but silent so
`modetest` can take DRM master itself:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1 hotplug_events=0
```

For this path the card must be kept off the compositor's seat, otherwise
KWin/Xwayland grab DRM master first. Install the development udev rule, which is
**isolated-testing only** — it removes the seat assignment, which also stops the
compositor from adopting the output:

```bash
sudo make install-dev-udev   # then log out/in once if KWin already opened the card
```

`scripts/test-driver-zero-copy.sh` automates this: it reloads an isolated
module, drives a `modetest` producer, and verifies the DMA-BUF/sync_file path.

Other validation scripts (run as root, in the virtme-ng VM or on the host):

- `scripts/vm-pacing-test.sh` — asserts the vblank timer fires at exactly
  60/120/144 Hz with no missed vblanks (uses `hermes-vblank-meter.c`);
- `scripts/vm-export-stress.sh` — hammers `ACQUIRE_FRAME` from many threads
  while the scanout buffer churns, to catch dma-buf/fence lifetime bugs (run
  under `slub_debug=FZPU`);
- `scripts/host-vblank-isolated.sh` — isolated vblank-rate check on real
  hardware; `scripts/host-restore-display.sh` re-enables the physical monitor.

Remove the rule to return to the compositor-driven path:

```bash
sudo make uninstall-dev-udev
```

The seat exclusion cannot live inside the kernel driver: `seat`,
`master-of-seat`, `ID_SEAT`, and `ID_AUTOSEAT` are udev/logind userspace
policy. The dev udev rule also sets `GROUP="video"`, `MODE="0660"`, and
`TAG+="uaccess"` so Hermes control ioctls can run without root.

## Userspace communication

Hermes-KMS exposes a small DRM ioctl UAPI in [include/uapi/drm/hermes_kms_drm.h](include/uapi/drm/hermes_kms_drm.h).

Initial ioctls:

- `DRM_IOCTL_HERMES_KMS_GET_VERSION`
- `DRM_IOCTL_HERMES_KMS_GET_IDENTITY`
- `DRM_IOCTL_HERMES_KMS_GET_CAPS`
- `DRM_IOCTL_HERMES_KMS_GET_STATUS`
- `DRM_IOCTL_HERMES_KMS_SET_OUTPUT`
- `DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME`
- `DRM_IOCTL_HERMES_KMS_WAIT_FRAME`
- `DRM_IOCTL_HERMES_KMS_GET_METRICS`

`GET_IDENTITY` exposes the stable Hermes-facing output name `HERMES-1` while
the DRM core may still expose the connector object as `Virtual-*`.

`GET_STATUS` now reports scanout/frame metadata:

- frame sequence counter;
- active session ID and owner pid when a control fd owns the output;
- last update/enable/disable timestamps;
- framebuffer ID;
- framebuffer width/height;
- DRM fourcc format;
- modifier;
- per-plane pitch/offset.

`SET_OUTPUT` is an `IOWR` ioctl. On enable or disable it returns the applied
mode, result flags, and the session ID owned by that fd. Hermes should keep the
same fd open for the whole stream; if that fd closes, the driver disconnects the
output, clears the tracked frame, and emits hotplug.

`ACQUIRE_FRAME` supports metadata-only acquisition by default. If userspace sets `HERMES_KMS_FRAME_REQUEST_DMABUF`, the driver exports DMA-BUF fds for the currently tracked scanout framebuffer and sets `HERMES_KMS_FRAME_DMABUF_VALID` on success.

If userspace sets `HERMES_KMS_FRAME_REQUEST_SYNC_FILE`, `ACQUIRE_FRAME`
returns a sync_file fd carrying the framebuffer's implicit write fence (from the
buffer's `dma_resv`), or an already-signalled fence when the buffer is idle. The
consumer waits on it before sampling, so a frame the compositor flipped while its
GPU was still rendering is read only after that render completes.

When the compositor supplies damage (`FB_DAMAGE_CLIPS`), `ACQUIRE_FRAME` also
returns the merged dirty rectangle (`damage_x1..y2`, flagged by
`HERMES_KMS_FRAME_DAMAGE_VALID`) so the consumer can encode only the changed
region. With no damage the whole frame is treated as dirty.

`WAIT_FRAME` lets Hermes block until `frame_sequence` advances past a known
sequence, with a caller-provided timeout. This is the intended low-latency
capture loop:

```text
WAIT_FRAME(after_sequence, timeout_ms)
ACQUIRE_FRAME(HERMES_KMS_FRAME_REQUEST_DMABUF | HERMES_KMS_FRAME_REQUEST_SYNC_FILE)
import DMA-BUF into encoder
after_sequence = returned sequence
```

This hands userspace a shared buffer with no driver-side CPU readback. Hermes
validates end-to-end zero-copy on VAAPI today (XRGB8888, linear): the captured
DMA-BUF is imported by a real GPU and encoded directly. NVENC/AMF still need
their own format/modifier/import validation.

`GET_METRICS` reports counters and timestamps for frame updates, frame waits,
acquires, DMA-BUF exports, sync_file exports, hotplug events, output lifecycle,
and owner-fd cleanup.

The driver deliberately does not advertise `writeback_connector` yet. That
needs real DRM writeback connector plumbing, not placeholder flags.

Diagnostic command:

```bash
tools/hermes-kmsctl/hermes-kmsctl frame
tools/hermes-kmsctl/hermes-kmsctl frame --require-dmabuf --sync-file
tools/hermes-kmsctl/hermes-kmsctl wait 0 1000
tools/hermes-kmsctl/hermes-kmsctl metrics
tools/hermes-kms-import-check/hermes-kms-import-check --wait-ms 1000
```

`hermes-kms-import-check` acquires the latest Hermes-KMS DMA-BUF frame and tries
to import it into VAAPI through `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`. This
is an encoder-path preflight: success means the VA driver accepted the exported
buffer as a `VASurface`; failure means Hermes will need a different scanout
format, modifier, GPU placement, or a conversion path before true encoder
zero-copy is possible.

The zero-copy test script can run the same preflight while its `modetest`
producer is still active:

```bash
./scripts/test-driver-zero-copy.sh --keep-loaded --check-vaapi-import
./scripts/test-driver-zero-copy.sh --keep-loaded --check-vaapi-import --va-device /dev/dri/renderD128
```

## Roadmap

See [docs/roadmap.md](docs/roadmap.md) and [docs/driver-design.md](docs/driver-design.md).

## License

Hermes-KMS is licensed under the **GNU General Public License, version 2**
(GPL-2.0) — the same license as the Linux kernel, which an out-of-tree DRM/KMS
module must use. See [LICENSE](LICENSE) for the full text; sources carry
`SPDX-License-Identifier: GPL-2.0`.

In short: you can use, modify, and redistribute it freely, and you can fork it.
Any distributed fork or derivative must remain under GPL-2.0 and ship its source
— it cannot be made proprietary. (Note that, like all GPL software, the license
governs the freedoms above; it does not by itself forbid charging for copies, but
recipients always keep the right to the source and to redistribute.)
