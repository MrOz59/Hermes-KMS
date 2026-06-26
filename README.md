# Hermes-KMS

Hermes-KMS is an experimental Linux DRM/KMS virtual display driver for Hermes.

The goal is to replace EVDI for Hermes with a virtual display backend that can eventually support a lower-latency DMA-BUF / PRIME path into hardware encoders.

This repository is local-only for now. No GitHub remote is configured.

## What this is

- A Linux kernel DRM/KMS driver project.
- A future EVDI replacement for Hermes.
- A virtual display target intended to expose a Hermes-owned output such as `HERMES-1`.
- A foundation for GPU-native buffer sharing and encoder import.

## What this is not

- It is not an EVDI manager.
- It is not a userspace wrapper around EVDI.
- It is not integrated into Hermes yet.
- It is not end-to-end encoder zero-copy validated yet.

## Current milestone

The current code starts the real driver path:

- out-of-tree kernel module target: `hermes_kms.ko`;
- DRM device registration skeleton;
- virtual connector using DRM/KMS helpers;
- simple display pipe;
- GEM shmem helper base for first functional buffers;
- Hermes/apps communication UAPI through DRM ioctls;
- scanout/frame metadata tracking;
- DMA-BUF export for the currently tracked scanout GEM framebuffer;
- debug/control tool: `tools/hermes-kmsctl/hermes-kmsctl`;
- virtual output defaults to disconnected until a control/session fd enables it;
- development udev rule for keeping local test cards off the active logind seat;
- 640x480 through 3840x2160 mode range, with 1920x1080 preferred;
- no Hermes integration yet.

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

Loading an unsigned experimental kernel module can destabilize the session. Use this only on a test machine.

KDE/KWin opens DRM/KMS cards that appear on the active logind seat. During
driver development this can keep `hermes_kms` in use and block `rmmod`.
Install the dev udev rule before repeated load/unload testing:

```bash
sudo make install-dev-udev
```

If KWin already opened the Hermes-KMS card, install the rule, log out and back
in once, then unload the old module:

```bash
sudo rmmod hermes_kms
```

Remove the rule when you want desktop compositors to see Hermes-KMS again:

```bash
sudo make uninstall-dev-udev
```

Use `insmod` from the repo directory while the module is still local-only:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko
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

By default, Hermes-KMS creates the DRM device with its virtual connector
disconnected. This prevents desktop compositors such as KWin from taking the
test display immediately on module load. A userspace owner must enable the
output through `SET_OUTPUT`; `hermes-kmsctl hold 1920x1080@60` keeps that owner
fd open until Ctrl+C. This follows the EVDI-style model: control ioctls use the
primary DRM node, the connector starts disconnected, and the compositor is only
notified when userspace deliberately exposes a display. The connector is marked
`non-desktop` so desktop sessions should ignore it unless explicitly
configured. If the owner fd closes or the process crashes, the driver
disconnects the virtual output and emits a hotplug event.

For isolated `modetest` validation, load with `initial_enabled=1
hotplug_events=0`. This exposes a connected connector without notifying the
desktop session, so `modetest` can become DRM master and commit a primary-plane
framebuffer.

The seat exclusion cannot live inside the kernel driver: `seat`,
`master-of-seat`, `ID_SEAT`, and `ID_AUTOSEAT` are udev/logind userspace
policy. The driver controls connector state and hotplug timing. The development
udev rule keeps the DRM primary node away from compositor seat auto-pickup while
setting `GROUP="video"`, `MODE="0660"`, and `TAG+="uaccess"` so Hermes control
ioctls can run without root once normal device permissions are in place.

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
returns a signaled sync_file fd for the tracked frame. This fence means the
frame reached the Hermes-KMS atomic update path and is available to consume; it
does not prove the downstream hardware encoder can import the buffer without an
internal copy.

`WAIT_FRAME` lets Hermes block until `frame_sequence` advances past a known
sequence, with a caller-provided timeout. This is the intended low-latency
capture loop:

```text
WAIT_FRAME(after_sequence, timeout_ms)
ACQUIRE_FRAME(HERMES_KMS_FRAME_REQUEST_DMABUF | HERMES_KMS_FRAME_REQUEST_SYNC_FILE)
import DMA-BUF into encoder
after_sequence = returned sequence
```

This proves the driver can hand userspace a shared buffer without a driver-side CPU readback. It does not, by itself, prove end-to-end hardware encoder zero-copy; Hermes still needs to validate format/modifier/import compatibility with VAAPI/NVENC/AMF.

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

See [hermes-linux-virtual-display-backend-roadmap.md](hermes-linux-virtual-display-backend-roadmap.md) and [docs/driver-design.md](docs/driver-design.md).
