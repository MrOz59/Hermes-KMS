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
- A virtual display that exposes independently controlled outputs
  (`HERMES-1`, `HERMES-2`, ...) which a desktop
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
2. A userspace owner opens the **render node** (never the primary node, which
   would steal DRM master and EBUSY-block the compositor), claims an output,
   and obtains an opaque session capability.
3. The compositor (KWin/GNOME) takes DRM master on the primary node, enables
   the now-connected `HERMES-1` output, and scans out the desktop into its
   framebuffer.
4. The owner hands the capability to a capture fd over an application-defined
   trusted channel. The bound fd pulls the current scanout as DMA-BUFs via
   `ACQUIRE_FRAME`; the kernel never checks an application or process name.
5. A real GPU imports those DMA-BUFs and encodes them. The frame never leaves
   the GPU.

Measured capture cost on KWin at 720p: ~8 us/frame (`ACQUIRE_FRAME`) versus
~180 us/frame for EVDI's CPU copy, and constant regardless of resolution.

## Using it in other projects

The driver is not Hermes-specific. "Hermes" here is the reference consumer, not
a requirement: there is no hard-coded process gating, no exported Hermes-only
symbol, and the capture path is a plain DRM ioctl UAPI over the render node. Any
userspace consumer — another streaming host such as Apollo/Sunshine, a recorder,
or your own tool — can use it by talking the same UAPI, with no fork required:

1. Open a candidate render node and identify it with the DRM core
   `DRM_IOCTL_VERSION` ioctl (or libdrm's `drmGetVersion()`); require the exact
   driver name `hermes-kms` before issuing any Hermes-KMS private ioctl. Private
   DRM command numbers are driver-local and are not safe device probes. Then use
   `GET_VERSION` / `GET_CAPS`. The current interface is UAPI v11; require
   `HERMES_KMS_CAP_SESSION_TOKEN` before using the protected capture path.
2. For UAPI v8 multi-output discovery/control, select an available output on
   the prospective owner fd with `SELECT_OUTPUT`; use one owner fd per
   simultaneous virtual display. An unselected fd is scoped to the first
   output, but still needs UAPI v11 authorization for protected operations.
3. For UAPI v9 independent sessions, discover every Hermes DRM card through
   `GET_IDENTITY.device_index/device_count` and give each compositor a
   different primary node. Each card is a separate DRM-master domain.
   With UAPI v10, prefer cards whose identity role is `SESSION`; their
   `session_index` maps directly to the packaged `hermes-kms-N` seat and
   broker. Session-role primary nodes are root-only and are opened through
   that broker; the `HOST` card remains available to the normal desktop.
4. On an owner fd, call `SET_OUTPUT` to claim and configure an available output
   (this does not take DRM master, so the compositor keeps it), then use
   `SESSION_ACCESS(GET_TOKEN)` to retrieve its opaque token and session ID.
5. Transfer the token through a trusted channel defined by your project. On
   each separate capture fd, call `SESSION_ACCESS(BIND)` with the output index,
   token and session ID. `BIND` selects the active output and authorizes the fd
   atomically; do not call `SELECT_OUTPUT` first on that capture fd.
6. A primary-frame-only consumer runs `WAIT_FRAME` → `ACQUIRE_FRAME` and
   imports the returned DMA-BUF into its encoder.
7. A consumer that needs the hardware cursor requires
   `HERMES_KMS_CAP_CURSOR_CAPTURE`, waits on both streams with `WAIT_UPDATE`,
   acquires the current primary frame and cursor with `ACQUIRE_FRAME` and
   `ACQUIRE_CURSOR`, waits on their sync_file fds, and composites the cursor.
   `GET_STATUS` and `GET_METRICS` use the same binding.
8. Disable the output or close its owner fd to revoke the capability and end
   the session.

The capability is deliberately generic: the driver neither knows nor accepts
an executable name, Steam application ID, UID, TGID, or Hermes-specific
credential. This also lets a broker hand capture access to a sandboxed worker
without baking that broker's policy into the kernel ABI. Treat the 128-bit
token as a secret and do not put it in logs or command lines.

[tools/hermes-kmsctl](tools/hermes-kmsctl) and
[tools/hermes-kms-import-check](tools/hermes-kms-import-check) are small,
self-contained reference consumers you can read or copy. The UAPI is documented
in [include/uapi/drm/hermes_kms_drm.h](include/uapi/drm/hermes_kms_drm.h) and in
[Userspace communication](#userspace-communication) below.

What is currently fixed (and would need a fork to rebrand, though not to use):
the output name prefix is `HERMES-` and the UAPI symbols are prefixed
`HERMES_KMS_`.

What is currently validated: VAAPI with `XRGB8888`, linear. NVENC/AMF and
NV12/P010 are not validated yet — see the [Roadmap](#roadmap). HDR advertisement
is now implemented but not yet validated end to end: with `hdr_enable=1` the
synthetic EDID's CTA-861 extension carries both an HDR Static Metadata Data Block
and a BT2020 Colorimetry Data Block, and the connector exposes both the
`HDR_OUTPUT_METADATA` and `Colorspace` properties — the three signals a
compositor needs together, since KWin gates HDR behind a wide-colour-gamut
prerequisite (the `Colorspace` property plus BT2020 in the EDID) before it will
enable high dynamic range. Full HDR streaming and the `color_depth=10` dependency
are still open. Forks and contributions extending those paths are welcome under
the project's license.

The kernel module is GPL-2.0, while the installed UAPI has the Linux syscall-note
exception and the optional userspace session helper is MIT-licensed. This keeps
an independently developed consumer separate from the Hermes application; see
[License](#license).

## Features

- out-of-tree kernel module: `hermes_kms.ko`;
- explicit CRTC/encoder/plane modeset with a software vblank timer, so the
  compositor composes the virtual output at its full refresh (60/120/144 Hz);
- cursor plane so the compositor offloads pointer motion, plus a separately
  sequenced cursor-capture stream for consumers that composite it themselves;
- damage tracking (`FB_DAMAGE_CLIPS`) forwarded to the capture consumer via
  `ACQUIRE_FRAME`;
- render node for masterless, zero-copy frame consumption;
- synthetic EDID 1.4 so compositors treat `HERMES-1` as a normal
  continuous-frequency monitor, with range limits derived from the configured
  mode envelope;
- 1–8 independent outputs on one DRM card (`outputs=`, default 1 for
  compatibility), with separate KMS pipelines, sessions, frame channels, and
  EDID serials;
- runtime card creation through configfs
  (`mkdir /sys/kernel/config/hermes-kms/<name>`), so a host adds and removes
  virtual displays on demand instead of drawing from a pool fixed at module
  load. Role and session index are settable before the card exists, so a
  dynamic card lands on the same seat and broker a pool card would, and the
  `card`/`render_node` attributes report the nodes it received;
- per-card render-node ownership (`access_uid`), so several mutually untrusted
  consumers can each hold their own card instead of sharing the one uid the
  packaged pool grants every Hermes node;
- 1–8 independent DRM cards (`devices=`, default 1), each with its own
  DRM-master domain so separate compositors can back separate graphical
  sessions. The legacy `devices=N outputs=1` layout remains available;
- UAPI v10 automatic session pools (`session_devices=1..8`). A pool adds one
  host card that remains on seat0 plus the requested private cards, so the
  existing host-desktop path and isolated sessions can coexist without
  reloading the module. The packaged default is four inert session cards.
  Scanout buffers are allocated only while a client owns a card;
- role-aware udev rules map only private cards to stable
  `hermes-kms-1..8` seats and ask systemd to start the matching private seat
  broker automatically;
- exact requested mode synthesized from CVT timings and re-probed on
  `SET_OUTPUT`, preserving non-eight-aligned visible widths such as 854 pixels
  while keeping the framebuffer pitch independently aligned for DMA-BUF;
- eight- and ten-bit scanout formats (`XRGB8888`/`ARGB8888` and the `2101010`
  variants), with the advertised EDID depth selected by `color_depth=`;
- optional HDR advertisement (`hdr_enable=1`, default off), which adds both a
  CTA-861 HDR Static Metadata Data Block and a BT2020 Colorimetry Data Block to
  the synthetic EDID and attaches both the `HDR_OUTPUT_METADATA` and `Colorspace`
  (advertising BT2020) connector properties together, so a compositor sees all
  three signals it needs to treat the output as HDR-capable — KWin, for example,
  requires the `Colorspace` property plus BT2020 in the EDID for its
  wide-colour-gamut prerequisite before it will enable HDR. Untested together
  with `color_depth=10`; validate that pairing before deployment;
- scanout modifier pass-through: any tiled or compressed layout the compositor's
  render GPU produces is accepted, and `scanout_modifiers=` publishes the extra
  layouts an `IN_FORMATS`-driven compositor can negotiate;
- DMA-BUF export of the tracked scanout framebuffer, cached per buffer object;
- real `dma_resv` write fence exported as a sync_file;
- UAPI v11 generic, opaque session-capability handoff between an output owner
  and one or more capture fds, with no application/process-name coupling;
- token rotation and one-shot revocation of every binding, so an owner can cut
  off a consumer without tearing down its stream, plus live binding counts and
  bind/reject/revoke metrics;
- fixed-layout DRM ioctls for frame acquisition, damage, synchronization and
  metrics, including vblank and late-vblank counters;
- debugfs telemetry at `/sys/kernel/debug/dri/<n>/hermes_kms_stats`;
- debug/control tool: `tools/hermes-kmsctl/hermes-kmsctl`, whose `hold` accepts
  `--control PATH` to rotate or revoke the session capability while it runs;
- configurable mode envelope, 640x480 through 7680x4320 at up to 240 Hz by
  default with 1920x1080 preferred, reported through `GET_CAPS` and reflected in
  the synthetic EDID's range limits;
- optional reported physical panel size, so a compositor can derive a scale.

## Build

Install matching kernel headers first. The default target builds only the
kernel module and therefore does not require the optional diagnostic-tool
libraries:

On CachyOS/Arch-like systems:

```bash
make
```

To build the module and every diagnostic userspace tool, install the libdrm,
VAAPI, GBM, EGL and OpenGL development packages, then run:

```bash
make full
```

Clean:

```bash
make clean
```

### Install

The Arch/CachyOS package installs DKMS, `seatd`, module-load/modprobe defaults,
the role-aware udev rule, and the private broker units together:

```bash
makepkg -si
```

The package loads one host card plus a four-card private session pool on boot.
Hermes' Audio/Video page can configure broker ownership for its current user
with one administrator prompt. The equivalent command is:

```bash
sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto
```

For a source installation, `sudo make dkms-install` now installs the same
runtime files. If an older module topology is already loaded, the helper saves
the new configuration and asks for one reboot instead of forcibly unloading a
card that a compositor may still have open.

### Image-based systems (Bazzite, Silverblue, SteamOS)

DKMS does not work on an image-based distribution. Its whole model is to rebuild
the module on the installed system whenever the kernel changes, and there `/usr`
is read-only at runtime, so that moment never arrives. The module has to be
built into the image instead.

`packaging/bazzite/Containerfile` does that: it compiles `hermes_kms.ko` against
the image's kernel and installs it to `/usr/lib/modules/<kver>/extra`, which is
where akmods and the ublue-os kmod images put theirs.

```bash
podman build -t localhost/bazzite-hermes:latest -f packaging/bazzite/Containerfile .
sudo bootc switch --transport containers-storage localhost/bazzite-hermes:latest
```

Reboot into the new image and the module autoloads. Push the image to a registry
and rebase from there if you want it to follow Bazzite's own updates.

**Secure Boot.** Bazzite ships with Secure Boot enabled, signed with the
Universal Blue key. A module built this way is unsigned, and an enforcing Secure
Boot will refuse to load it. Either enroll your own MOK and pass it to the build:

```bash
podman build \
  --no-cache \
  --secret id=module_sign_key,src=/path/to/MOK.priv \
  --secret id=module_sign_cert,src=/path/to/MOK.der \
  -t localhost/bazzite-hermes:latest -f packaging/bazzite/Containerfile .
```

Both secrets are required together. They are mounted only in the discarded
build stage and are never copied into the build context or final image.

…or turn Secure Boot off in firmware. There is no way around that pair.

The two Makefile targets behind it work for any image build, not only Bazzite:

```bash
make modules KERNELRELEASE=<kver> KDIR=/usr/lib/modules/<kver>/build
make modules-install install-configs KERNELRELEASE=<kver> DESTDIR=/
```

`modules-install` places the module and runs `depmod`; `install-configs` adds the
module options, the autoload entry, the session-seat udev rule and the seatd
helper under `/usr`, so they survive on a read-only root. On a staging `DESTDIR`
with no full module tree `depmod` is skipped with a note rather than failing —
run `depmod -a <kver>` on the target in that case.

## Local test commands

Loading an unsigned experimental kernel module can destabilize the session. Use
this only on a test machine. Two ways to drive the output are described below:
compositor-driven (real streaming) and isolated `modetest` (driver validation).

Inspect the driver with the control tool while it is loaded:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=0 outputs=2
sleep 1
tools/hermes-kmsctl/hermes-kmsctl version
tools/hermes-kmsctl/hermes-kmsctl outputs
tools/hermes-kmsctl/hermes-kmsctl identity
tools/hermes-kmsctl/hermes-kmsctl --output 2 identity
tools/hermes-kmsctl/hermes-kmsctl caps
tools/hermes-kmsctl/hermes-kmsctl status
tools/hermes-kmsctl/hermes-kmsctl --verbose status
ls -l /dev/dri/
modetest -c
drm_info
journalctl -k -g hermes-kms
sudo rmmod hermes_kms
```

Active output state and capture are capability-protected. For command-line
diagnostics, keep an owner running in one terminal and publish its credential
to a same-UID, mode-0600 file under the user's runtime directory:

```bash
session_file="${XDG_RUNTIME_DIR:?}/hermes-kms-session"
tools/hermes-kmsctl/hermes-kmsctl --output 1 \
  --session-file "$session_file" hold 1920x1080@60
```

While that command holds the owner fd open, use the file from another terminal:

```bash
session_file="${XDG_RUNTIME_DIR:?}/hermes-kms-session"
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" status
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" metrics
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" wait 0 1000
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" \
  frame --require-dmabuf --sync-file
```

`hermes-kmsctl` removes its diagnostic credential file when the owner exits.
Production consumers should normally transfer the token in memory over their
own trusted IPC rather than writing it to disk.

Optional initial mode/state parameters:

```bash
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_width=1920 initial_height=1080 initial_refresh_hz=60
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=1
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=0 outputs=2
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=0 devices=2 outputs=1
sudo insmod kernel/hermes-kms/hermes_kms.ko initial_enabled=0 session_devices=4 outputs=1
```

`hdr_enable=1` advertises HDR (default off). It gates three mechanisms together —
the CTA-861 HDR Static Metadata Data Block and the BT2020 Colorimetry Data Block
in the EDID, and the `HDR_OUTPUT_METADATA` and `Colorspace` connector
properties — so the output never advertises a subset. All three are needed
because KWin treats an output HDR-capable through a two-gate chain: it sets
`WideColorGamut` only when the connector has a `Colorspace` property advertising
BT2020 and the EDID reports BT2020 (the Colorimetry block), and only then sets
`HighDynamicRange`, which additionally needs `HDR_OUTPUT_METADATA` and the EDID's
PQ HDR Static Metadata block. An earlier attempt carrying only the HDR Static
Metadata block and `HDR_OUTPUT_METADATA` left HDR off on hardware precisely
because the Colorimetry block and `Colorspace` property were missing. Whether
these advertisements alone are enough for a compositor to enable HDR, or whether
ten-bit scanout (`color_depth=10`) must be set at the same time, is not yet
confirmed; the combination is untested together and must be validated before
deployment. To load HDR together with ten-bit scanout:

```bash
sudo modprobe hermes_kms color_depth=10 hdr_enable=1
```

To apply the same parameters persistently at every module load, drop a file in
`/etc/modprobe.d`:

```
# /etc/modprobe.d/hermes-kms-hdr.conf
options hermes_kms color_depth=10 hdr_enable=1
```

During verification, compare two configurations to determine empirically whether
the advertisement alone is sufficient: (a) `hdr_enable=1` without `color_depth=10`,
and (b) `hdr_enable=1` with `color_depth=10`. If a consumer still reports
"HDR: incapable" after all three mechanisms are applied and `color_depth=10` is
set, CRTC color-management properties are the next investigation area — that is
outside this feature's scope and no further code change is made here.

Independent compositors use the packaged
`72-hermes-kms-session-seats.rules` and one private seat broker per session
device. The udev rule starts those instances as cards appear; users no longer
enable N units or join the `seat` group manually. `hermes-kms-setup` records
the Hermes service uid, removes persistent broker enablement left by older
prototype instructions, stops instances outside the configured pool, and
restarts the required brokers with sockets owned by that user.

The sockets are exposed as
`/run/hermes-kms-seatd/1/seatd.sock`,
`/run/hermes-kms-seatd/2/seatd.sock`, and so on. Each service runs the stock
`seatd` daemon inside its own mount namespace because the daemon has a
compile-time socket path and only permits one active compositor per daemon.
The pool's host-role card stays on seat0, so existing KWin/GNOME use is
unchanged. These private brokers are experimental session plumbing, not a
security boundary between mutually untrusted local users.

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

- `scripts/vm-session-lifecycle-test.sh` — checks token rotation, one-shot
  binding revocation, binding accounting, and that a blocked `WAIT_FRAME` on a
  revoked descriptor is woken with `EACCES` rather than left to time out;
- `scripts/vm-configfs-test.sh` — creates and removes cards at runtime through
  configfs alongside a static card, and verifies role/seat metadata, unique
  output naming, rejected writes on a live card, refused unload, and clean
  teardown;
- `scripts/vm-multi-device-test.sh` — creates two independent DRM cards, gives
  each one a simultaneous DRM master, and verifies distinct owner/frame/DMA-BUF
  channels, exact 854x480 visible geometry with an aligned pitch, and clean
  teardown;
- `scripts/vm-multi-compositor-test.sh` — starts two private seat brokers and
  two Weston DRM compositors concurrently, one on each card, then verifies
  independent scanout modes;
- `scripts/vm-multi-output-test.sh` — drives two outputs concurrently at
  different modes and verifies distinct owners, framebuffers, DMA-BUFs,
  independent disconnect, and clean unload;
- `scripts/vm-session-pool-test.sh` — validates one seat0 host card, private
  role/index identities, automatic seat assignments, and systemd broker wants
  for a two-card session pool;
- `scripts/vm-setup-helper-test.sh` — validates one-click user/topology
  persistence, exact broker restarts, and the safe reboot-required upgrade
  path;
- `scripts/vm-systemd-broker-test.sh` — boots a systemd guest and validates the
  packaged unit's hardening plus configured-user socket ownership;
- `scripts/vm-uapi-v7-compat-test.sh` — verifies an unmodified v0.1.2 control
  client remains confined to the first output under the current UAPI;
- `scripts/vm-pacing-test.sh` — asserts the vblank timer fires at exactly
  60/120/144 Hz with no missed vblanks (uses `hermes-vblank-meter.c`);
- `scripts/vm-export-stress.sh` — hammers `ACQUIRE_FRAME` from many threads
  while the scanout buffer churns, to catch dma-buf/fence lifetime bugs (run
  under `slub_debug=FZPU`);
- `scripts/host-vblank-isolated.sh` — isolated vblank-rate check on real
	  hardware; `scripts/host-restore-display.sh` re-enables a caller-selected
	  physical monitor and can stop caller-selected streaming processes gracefully.

For example, from SSH or a TTY:

```bash
scripts/host-restore-display.sh --physical HDMI-A-1 --virtual Virtual-1 \
  --stop my-stream-server
```

Remove the rule to return to the compositor-driven path:

```bash
sudo make uninstall-dev-udev
```

The seat exclusion cannot live inside the kernel driver: `seat`,
`master-of-seat`, `ID_SEAT`, and `ID_AUTOSEAT` are udev/logind userspace
policy. The dev udev rule also sets `GROUP="video"`, `MODE="0660"`, and
`TAG+="uaccess"` so a local test controller can run without root.

## Userspace communication

Hermes-KMS exposes a small DRM ioctl UAPI in
[include/uapi/drm/hermes_kms_drm.h](include/uapi/drm/hermes_kms_drm.h).
External projects can install the public UAPI and the application-neutral MIT
session helper without installing any Hermes application code:

```bash
sudo make install-uapi
```

```c
#include <drm/hermes_kms_drm.h>
#include <hermes-kms/hermes_session.h>  /* optional header-only helper */
```

Both paths honor `DESTDIR`, `PREFIX`, `UAPI_INCLUDE_DIR`, and
`HERMES_INCLUDE_DIR`; `sudo make uninstall-uapi` removes only those two managed
headers. Consumers using the helper also need the normal libdrm and Linux UAPI
headers available at compile time.

The kernel/module sources remain GPL-2.0, and the public UAPI carries the Linux
syscall-note exception. The standalone session helper and its userspace-only
tests are MIT-licensed under [LICENSES/MIT.txt](LICENSES/MIT.txt), so another
project can reuse that helper without inheriting application-specific code.

Before calling any ioctl listed below, identify the fd with the DRM core
`DRM_IOCTL_VERSION` ioctl (or `drmGetVersion()`) and require the exact driver
name `hermes-kms`. Do not use a private ioctl as the initial probe: private DRM
command numbers have meaning only after the core driver identity is known and
can overlap with another driver's commands.

The current interface is UAPI v11. Its ioctls are:

- `DRM_IOCTL_HERMES_KMS_GET_VERSION`
- `DRM_IOCTL_HERMES_KMS_GET_IDENTITY`
- `DRM_IOCTL_HERMES_KMS_GET_CAPS`
- `DRM_IOCTL_HERMES_KMS_GET_STATUS`
- `DRM_IOCTL_HERMES_KMS_SET_OUTPUT`
- `DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME`
- `DRM_IOCTL_HERMES_KMS_WAIT_FRAME`
- `DRM_IOCTL_HERMES_KMS_GET_METRICS`
- `DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT` (UAPI v8)
- `DRM_IOCTL_HERMES_KMS_SESSION_ACCESS` (UAPI v11)
- `DRM_IOCTL_HERMES_KMS_ACQUIRE_CURSOR` (UAPI v11)
- `DRM_IOCTL_HERMES_KMS_WAIT_UPDATE` (UAPI v11)

`GET_CAPS.output_count` reports the number of independent outputs.
`SELECT_OUTPUT` binds all output-scoped ioctls on that fd to a 0-based output;
new fds default to output 0 for compatibility. `GET_IDENTITY` then exposes the
selected stable driver-facing name (`HERMES-1`, `HERMES-2`, ...) while the DRM
core may still expose connector objects as `Virtual-*`.
UAPI v9 also exposes `GET_IDENTITY.device_index` and `device_count`. UAPI v10
adds `device_role`, `session_index`, and `session_device_count` in words that
were previously reserved. Consumers must check
`HERMES_KMS_CAP_SESSION_DEVICE_POOL` before interpreting them.
`GET_IDENTITY.cursor_plane_id` identifies the cursor plane when
`HERMES_KMS_CAP_CURSOR_CAPTURE` is advertised.
`HERMES_KMS_CAP_MULTI_DEVICE` means the module can create multiple independent
DRM devices with `devices=N`; every device has its own DRM-master ownership
domain and contains `output_count` selectable outputs.

UAPI v11 advertises `HERMES_KMS_CAP_SESSION_TOKEN` and protects active output
state, frame capture, waits and metrics with a generic session capability.
`GET_STATUS` remains public while an output is completely idle so a controller
can discover and claim it. The fd that successfully calls `SET_OUTPUT` is the
owner and can call
`SESSION_ACCESS(GET_TOKEN)` with only the operation field set and every other
request field zero; the response returns its output index, 128-bit token,
session ID and `RESULT_TOKEN_VALID`. A second fd calls
`SESSION_ACCESS(BIND)` with those three values. `BIND` selects the active output
and authorizes the fd in one operation, returning `RESULT_BOUND` with the token
fields cleared; a preceding `SELECT_OUTPUT` is neither needed nor valid for
another session's active output. `UNBIND` removes access from that fd. Disabling
the output or closing the owner fd invalidates every binding for the old
session.

Token transport and policy belong to userspace. The kernel does not key access
to Hermes, Steam, an executable name, UID or process relationship; any project
can pass the opaque token through its own trusted IPC channel. Tokens are
credentials: keep them out of logs, arguments and world-readable files.

For short-lived migration of old diagnostic clients, root can load the module
with `insecure_legacy_unbound_access=1`. This restores unbound pre-v11 access
and therefore permits any process that can open the node to inspect/capture an
active session; it is intentionally off by default and is unsafe for normal use.

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
mode, result flags, and the session ID owned by that fd. The controller should
keep the same fd open for the whole stream; if it closes, the driver revokes the
token, disconnects the output, clears the tracked frame, and emits hotplug.
Real state changes are rate-limited to a burst of ten per output per second;
`EAGAIN` means no mutation occurred and may be retried with bounded backoff.
Exact repeats and an already-idle disable are not charged.

`ACQUIRE_FRAME` supports metadata-only acquisition by default. If userspace sets
`HERMES_KMS_FRAME_REQUEST_DMABUF`, the driver exports DMA-BUF fds for the
currently tracked scanout framebuffer and sets
`HERMES_KMS_FRAME_DMABUF_VALID` on success.

If userspace sets `HERMES_KMS_FRAME_REQUEST_SYNC_FILE`, `ACQUIRE_FRAME`
returns a sync_file fd carrying the framebuffer's implicit write fence (from the
buffer's `dma_resv`), or an already-signalled fence when the buffer is idle. The
consumer waits on it before sampling, so a frame the compositor flipped while its
GPU was still rendering is read only after that render completes.

When the compositor supplies damage (`FB_DAMAGE_CLIPS`), `ACQUIRE_FRAME` can
return the merged half-open dirty rectangle (`damage_x1..y2`, flagged by
`HERMES_KMS_FRAME_DAMAGE_VALID`). A consecutive new sequence can use that
rectangle; acquiring the same sequence again returns a valid empty rectangle,
meaning no additional primary-plane change. On the fd's first acquire, after a
skipped sequence, or when the compositor supplies no damage, the flag is clear
and the consumer must treat the whole frame as dirty. A cursor-only commit does
not advance the primary-frame sequence.

`WAIT_FRAME` lets a capture consumer block until `frame_sequence` advances past
a known sequence, with a caller-provided timeout. This is the intended
low-latency primary-only capture loop; cursor-only commits deliberately do not
wake it:

```text
WAIT_FRAME(after_sequence, timeout_ms)
ACQUIRE_FRAME(HERMES_KMS_FRAME_REQUEST_DMABUF | HERMES_KMS_FRAME_REQUEST_SYNC_FILE)
import DMA-BUF into encoder
after_sequence = returned sequence
```

### Separate cursor capture

When `HERMES_KMS_CAP_CURSOR_CAPTURE` is present, the hardware cursor is exposed
as a latest-state stream separate from the primary framebuffer. This matters
because a compositor can move its cursor plane without redrawing the primary
plane; such pixels are not present in the DMA-BUF returned by `ACQUIRE_FRAME`.
`WAIT_UPDATE` accepts the last consumed primary `frame_sequence` and cursor
`cursor_sequence`, then wakes when either advances. Its response sets
`HERMES_KMS_WAIT_UPDATE_FRAME_READY`,
`HERMES_KMS_WAIT_UPDATE_CURSOR_READY`, or both and samples the current
sequences, timestamps, and output status coherently under the same state lock.
With no pending update, a zero timeout returns `EAGAIN` and a nonzero timeout
expires with `ETIMEDOUT`; session revocation takes precedence as `EACCES`.

The two sequence spaces are independent. Coherent values in one `WAIT_UPDATE`
response do not mean that its frame and cursor sequence numbers identify one
atomic compositor transaction, and an acquire made afterward can observe an
even newer latest state. Consumers should treat each READY bit relative to the
corresponding sequence they supplied, retain the latest complete state of each
stream, and tolerate skipped values rather than treating the two counters as a
paired frame ID.

`ACQUIRE_CURSOR` returns cursor metadata by default and can export its DMA-BUF
and write-fence sync_file when the corresponding request flags are set. The
cursor plane uses `DRM_FORMAT_ARGB8888`; its RGB components follow the standard
DRM premultiplied-alpha convention and the plane opacity is full, so a software
consumer must not interpret the pixels as straight alpha or multiply RGB by
alpha a second time. Wait for the returned sync_file before reading or importing
the buffer.

`position_x/y` is the nominal, unclipped top-left of the full cursor image in
CRTC coordinates. When `HOTSPOT_VALID` is set, `hotspot_x/y` is relative to
that full image, so the logical pointer hotspot is
`(position_x + hotspot_x, position_y + hotspot_y)`. Hermes exposes a universal
cursor plane rather than requiring the para-virtualized DRM hotspot extension;
ordinary compositors therefore pre-adjust the plane position and the hotspot
validity bit remains clear.
`crtc_x/y/w/h` is the clipped integer destination rectangle, while
`src_x/y/w/h` is the matching clipped source rectangle in DRM 16.16 fixed-point
pixels. Use those clipped rectangles when the cursor crosses an output edge;
do not crop by changing the meaning of the hotspot. The validity flags say
which metadata groups may be consumed. `VISIBLE` is intentionally independent
of `BUFFER_VALID`, allowing an off-screen or hidden cursor to retain a reusable
image.

`cursor_sequence` advances for position, visibility, or image-state updates.
`image_sequence` advances conservatively whenever a committed cursor buffer may
need to be sampled again, including in-place pixel updates. Refresh cached pixels
when it changes. If a primary or cursor buffer is replaced while an fd export is
being prepared, `ACQUIRE_FRAME` or `ACQUIRE_CURSOR` can return `ESTALE`; discard
that attempt and retry the latest-state acquire with a small bounded loop. This
prevents a consumer from receiving an fd paired with stale metadata.

A combined loop therefore uses `WAIT_UPDATE`, refreshes whichever primary or
cursor state advanced, waits for requested fences, and composites the clipped
premultiplied cursor over the primary image. If the compositor uses a software
cursor instead of the KMS cursor plane, no separate visible cursor buffer is
reported because those pixels are already part of the primary framebuffer.

This hands userspace a shared buffer with no driver-side CPU readback. The
reference consumer validates end-to-end zero-copy on VAAPI today (XRGB8888, linear): the captured
DMA-BUF is imported by a real GPU and encoded directly. NVENC/AMF still need
their own format/modifier/import validation.

`GET_METRICS` reports counters and timestamps for frame updates, frame waits,
acquires, DMA-BUF exports, sync_file exports, hotplug events, output lifecycle,
and owner-fd cleanup. UAPI v11 also names the previously reserved
`vblank_count` and `vblank_overrun_count` words without changing the struct's
size. The overrun counter measures timer intervals missed when a vblank
callback runs late; it is not a count of rejected frame acquires.

The driver deliberately does not advertise `writeback_connector` yet. That
needs real DRM writeback connector plumbing, not placeholder flags.

Diagnostic command:

```bash
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" frame
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" \
  frame --require-dmabuf --sync-file
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" wait 0 1000
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" metrics
tools/hermes-kms-import-check/hermes-kms-import-check \
  --session-file "$session_file" --wait-ms 1000
```

`hermes-kms-import-check` acquires the latest Hermes-KMS DMA-BUF frame and tries
to import it into VAAPI through `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`. This
is an encoder-path preflight: success means the VA driver accepted the exported
buffer as a `VASurface`; failure means a consumer may need a different scanout
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

The kernel module and kernel-side sources are licensed under the **GNU General
Public License, version 2** (GPL-2.0), the license required for this out-of-tree
DRM/KMS module. See [LICENSE](LICENSE); those sources carry
`SPDX-License-Identifier: GPL-2.0`. GPL-2.0 terms apply when distributing a fork
or derivative of that kernel/module code, including the corresponding-source
obligations.

The installed public UAPI header instead carries
`GPL-2.0 WITH Linux-syscall-note`, matching the normal Linux userspace boundary.
The optional header-only session helper and its userspace-only tests carry the
MIT license in [LICENSES/MIT.txt](LICENSES/MIT.txt). An independently developed
userspace project therefore does not have to become the Hermes application or
adopt its application license merely because it issues the documented ioctls,
includes the syscall-note UAPI, or reuses the MIT helper.
