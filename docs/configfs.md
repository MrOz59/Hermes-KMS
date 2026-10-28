# Runtime virtual displays through configfs

Hermes-KMS can create DRM cards at runtime without reloading the module. This
interface is implemented on `main` and advertised by
`HERMES_KMS_CAP_DYNAMIC_DEVICES` (introduced in UAPI v12). It does not require
the Hermes application. Configfs writes require root; render-node access and
capture authorization remain separate steps.

## Create a generic display

For a runtime-only topology, load with `devices=0 session_devices=0`. These are
load-time options: if the module is already in use, configure them for the next
boot rather than unloading a live desktop card.

```bash
sudo modprobe hermes_kms devices=0 session_devices=0
mountpoint -q /sys/kernel/config || sudo mount -t configfs none /sys/kernel/config
```

The following creates a generic 1080p profile and assigns read-only render-node
access to the invoking user. Run it as that intended non-root user:

```bash
capture_uid="$(id -u)"
sudo sh -eu -s -- "$capture_uid" <<'CONFIG'
card=/sys/kernel/config/hermes-kms/recorder-1
mkdir "$card"
echo "$1" > "$card/access_uid"
echo 1920 > "$card/initial_width"
echo 1080 > "$card/initial_height"
echo 60 > "$card/initial_refresh_hz"
echo 0 > "$card/initial_enabled"
echo 1 > "$card/enabled"
cat "$card/card"
cat "$card/render_node"
CONFIG
```

`enabled=1` creates the DRM card; it does not connect the output when
`initial_enabled=0`. An owner calls `SET_OUTPUT` (or `hermes-kmsctl hold`) to
connect it and keeps that fd open. A standalone display project that does not
need the capture UAPI can instead set `initial_enabled=1` before enabling the
card, then let its compositor drive the output.

Configfs values exist in memory and disappear on reboot or group removal.
Recreate the profile through your controller/service when needed. Card and
render-node numbers are discovered after creation, not persisted identifiers.

## Attribute reference

Set configuration attributes before writing `enabled=1`. Writes while the card
is live return `EBUSY`; disable/recreate it to change its profile. Display
settings initially inherit module parameters, except for the generic identity.

| Attribute | Meaning and accepted values |
| --- | --- |
| `outputs` | 1–8 independent output pipelines on this card; default 1. |
| `role` | `general` (default), `host`, or `session`; controls published role and packaged seat policy. |
| `session_index` | 0–8; session-role cards require an unused 1–8 index. A live static pool or dynamic session card can already reserve it. |
| `access_uid` | Local account UID for the packaged read-only render-node ACL; 0 means no explicit assignment. An unknown account receives no grant. |
| `min_width`, `min_height` | Mode-envelope minima, each 64–16384; enable-time checks additionally require an EDID-encodable fallback and minima no greater than 4095. |
| `max_width`, `max_height` | Mode-envelope maxima, each 64–16384; must cover the corresponding minima and initial mode. |
| `max_refresh_hz` | 1–1000 Hz, also bounding the initial refresh. |
| `initial_width`, `initial_height` | Preferred/initial visible geometry, each 64–16384, validated against the envelope. Exact streaming modes may later be requested with `SET_OUTPUT`. |
| `initial_refresh_hz` | Initial refresh, 1–1000 Hz, within the envelope. |
| `physical_width_mm`, `physical_height_mm` | 0–2550 mm; set both or leave both 0 (undefined size). |
| `color_depth` | 8 or 10 bits per RGB primary (checked when enabling the card). This is EDID precision, not proof of HDR. |
| `hdr_enable` | Boolean; adds PQ/BT.2020 HDR advertisement and connector properties. The profile must include 640x480@60 for CTA conformity. |
| `initial_enabled` | Boolean; initial connector connection, default inherited from the module (normally false). Distinct from card creation. |
| `non_desktop` | Boolean; mark the connector non-desktop, subject to compositor policy. |
| `manufacturer` | Three uppercase ASCII letters; generic default `VRT`. This changes EDID identity, not the DRM protocol name. |
| `monitor_name` | 1–12 printable ASCII characters; generic default `Virtual KMS`. |
| `output_prefix` | 1–20 ASCII letters, digits, hyphens or underscores; generic default `VIRTUAL-`. |
| `serial_base` | 0 through `INT_MAX - 8`. Nonzero reserves consecutive output identities/EDID serials; 0 uses automatic allocation. The initial value is derived from the configfs group name. |
| `scanout_modifiers` | Comma-separated extra numeric modifiers (decimal or `0x`), at most 15, with no duplicates or `DRM_FORMAT_MOD_INVALID`. Linear is implicit and must not be repeated; an empty value clears extras. |
| `enabled` | Boolean; create/remove the DRM card. Invalid or inconsistent profiles fail on enable. |
| `card` | Read-only `cardN` name while enabled; empty while disabled. |
| `render_node` | Read-only `renderDN` name while enabled; empty while disabled. |
| `device_index` | Read-only current driver index; -1 before creation/after disable. Indices can be reused. |

`serial_base` supplies both the EDID serial and the numeric suffix in the friendly
output name. Group-name-derived defaults keep a recreated profile's identity
consistent, but are not a collision-free registry. An explicit or derived serial
range that overlaps a live output fails card creation. Static cards keep their
`HERMES-` prefix; dynamic cards can be rebranded without changing any kernel code.
Always identify the driver as `hermes-kms` through `DRM_IOCTL_VERSION` before
issuing private ioctls, regardless of the configured display name.

To advertise ten-bit HDR10, set `color_depth=10` and `hdr_enable=1` before
creation. `ACQUIRE_FRAME2` carries the actual committed colour state; neither
setting forces the compositor into HDR. See [hdr-capture.md](hdr-capture.md).

## Private compositor cards and access policy

For a private card, set `role=session` and an unused `session_index=1..8` before
creation. The optional broker package maps that index to `hermes-kms-N` and
starts `hermes-kms-seatd@N.service`. Its socket is
`/run/hermes-kms-seatd/N/seatd.sock`. The packaged service uses
`SEATD_VTBOUND=0` so a private session does not take over the host's virtual
terminal.

`access_uid` controls render-node ACLs; it does not configure the seatd socket's
owner. The packaged broker reads its configured user from
`/etc/hermes-kms/session-user`. The setup helper configures a static session
pool as well as this user, so it is not a runtime-only profile manager. A custom
controller must arrange its broker/socket policy deliberately.

The core udev/ACL helper must be installed for `access_uid` to grant access.
Without it, the kernel only publishes the UID attribute; it does not change node
ownership itself. Open the capture node `O_RDONLY | O_CLOEXEC`. Session-role
primary nodes stay root-only and are accessed through the broker. An unassigned
dynamic general card does not automatically gain the static host's seat0 ACL.

Device ACLs control who can open a card. Session tokens control who can inspect
or capture an active output. Neither mechanism recalls DMA-BUF fds already
exported, isolates a compositor's reused buffers, nor turns a shared-UID session
pool into a security boundary. See [driver-design.md](driver-design.md#generic-session-capability).

## Teardown and checks

Stop capture and the private compositor before normal cleanup, then remove the
profile:

```bash
sudo sh -eu <<'CONFIG'
card=/sys/kernel/config/hermes-kms/recorder-1
echo 0 > "$card/enabled"
rmdir "$card"
CONFIG
```

`rmdir` on an enabled group also hot-unplugs the card. Open DRM descriptors may
survive removal and return `ENODEV`; close them rather than reusing them for a
new card with a recycled node number. Existing configfs groups hold a module
reference, so remove all groups before unloading the module.

`scripts/vm-configfs-test.sh` exercises profiles, identity, role/index mapping,
access policy, runtime-only topology and teardown in a disposable VM. See
[the upstream configfs/VKMS references](references.md#kernel-and-buffer-contracts)
for the underlying mechanism; Hermes-KMS attributes are defined in
[`hermes_kms.c`](../kernel/hermes-kms/hermes_kms.c).
