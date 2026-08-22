#!/bin/bash
# Cursor/plane test: GNOME/Mutter driving hermes-kms inside a virtme-ng VM.
#
# Loads the host-built hermes_kms.ko, starts a GNOME/Mutter session on the
# virtio-gpu, enables the virtual output, moves the pointer with ydotool and
# records whether Mutter uses the cursor plane and whether the independent
# cursor DMA-BUF stream reports coherent metadata and visible pixel content.
#
# Run inside the VM as root (copied to /opt/hermes-test/run-gnome-test.sh by the host).
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH
RESULTS=/host/results
SESSION_DIR=/run/hermes-kms-vm-test
SESSION_FILE="$SESSION_DIR/session.auth"
mkdir -p "$RESULTS" "$SESSION_DIR"
chmod 700 "$SESSION_DIR"
rm -f -- "$RESULTS/test.log" "$RESULTS/system-info.log" \
	"$RESULTS/imported-scanout.log" "$RESULTS/cursor-probe.log" \
	"$RESULTS/pixel-peek.log" "$RESULTS/ydotool.log" \
	"$RESULTS/ydotoold.log" "$RESULTS/gnome-shell.log" \
	"$RESULTS/mutter.log" "$RESULTS/svc.log" "$RESULTS/logind.log" \
	"$RESULTS/mksession.log" "$RESULTS/virgl.log" \
	"$RESULTS/egl-import.log" "$RESULTS/dmesg.log"
exec >"$RESULTS/test.log" 2>&1
set -x

LOGIND_PID=""
HOLD_PID=""
COMPO_PID=""
YDOTOOLD_PID=""
PROBE_PID=""
PEEK_PID=""
HERMES_LOADED=0
VIRTIO_RENDER=""

stop_pid() {
	local pid=$1

	[ -n "$pid" ] || return 0
	if kill -0 "$pid" 2>/dev/null; then
		kill "$pid" 2>/dev/null || true
		if ! timeout -k 1s 2s tail --pid="$pid" -f /dev/null \
			>/dev/null 2>&1; then
			kill -KILL "$pid" 2>/dev/null || true
		fi
	fi
	wait "$pid" 2>/dev/null || true
}

cleanup() {
	stop_pid "$PEEK_PID"
	stop_pid "$PROBE_PID"
	stop_pid "$YDOTOOLD_PID"
	stop_pid "$COMPO_PID"
	stop_pid "$HOLD_PID"
	stop_pid "$LOGIND_PID"
	PEEK_PID=""
	PROBE_PID=""
	YDOTOOLD_PID=""
	COMPO_PID=""
	HOLD_PID=""
	LOGIND_PID=""
	rm -f -- "$SESSION_FILE"
	dmesg >"$RESULTS/dmesg.log" 2>&1 || true
	if [ "$HERMES_LOADED" -eq 1 ]; then
		if timeout -k 1s 5s rmmod hermes_kms 2>/dev/null; then
			HERMES_LOADED=0
		else
			return 1
		fi
	fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# hermes-kmsctl intentionally publishes with no-replace semantics.  A token
# left by an interrupted previous run is revoked but would block publication.
rm -f -- "$SESSION_FILE"

# Make the requested guest environment part of the pass/fail contract instead
# of relying on the directory name used by the host harness.
if [ ! -r /etc/os-release ]; then
	echo "FAIL: guest has no /etc/os-release"
	echo FAILED > /host/status
	exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
{
	printf 'os_id=%s\n' "${ID:-unknown}"
	printf 'os_pretty_name=%s\n' "${PRETTY_NAME:-unknown}"
	printf 'kernel=%s\n' "$(uname -r)"
	sha256sum /opt/hermes-test/run-gnome-test.sh \
		/opt/hermes-test/hermes_kms.ko \
		/opt/hermes-test/hermes-imported-scanout-test \
		/opt/hermes-test/hermes-egl-import-check \
		/opt/hermes-test/hermes_cursor_probe \
		/opt/hermes-test/hermes_pixel_peek \
		/opt/hermes-test/hermes-kmsctl
	pacman -Q cachyos-keyring cachyos-mirrorlist cachyos-v3-mirrorlist \
		gnome-shell mutter mesa mesa-utils 2>&1
} >"$RESULTS/system-info.log"
if [ "${ID:-}" != cachyos ]; then
	echo "FAIL: expected a CachyOS guest, got ID=${ID:-unset}"
	echo FAILED > /host/status
	exit 1
fi
case "$(uname -r)" in
	*cachyos*) ;;
	*)
		echo "FAIL: guest kernel is not a CachyOS kernel"
		echo FAILED > /host/status
		exit 1
		;;
esac
if ! pacman -Q cachyos-keyring cachyos-mirrorlist cachyos-v3-mirrorlist \
	gnome-shell mutter mesa-utils \
	>>"$RESULTS/system-info.log" 2>&1; then
	echo "FAIL: CachyOS identity packages or GNOME Shell/Mutter are missing"
	echo FAILED > /host/status
	exit 1
fi

systemctl start dbus 2>>"$RESULTS/svc.log" || true
/usr/lib/systemd/systemd-logind >"$RESULTS/logind.log" 2>&1 &
LOGIND_PID=$!
sleep 2
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
modprobe drm 2>/dev/null || true
insmod /opt/hermes-test/uinput.ko 2>/dev/null || true
insmod /opt/hermes-test/virtio_dma_buf.ko 2>/dev/null || true
insmod /opt/hermes-test/virtio-gpu.ko 2>/dev/null || true
sleep 1

# The Mutter regression is meaningful only if its primary GPU is genuinely
# accelerated.  Probe it before loading Hermes-KMS so GBM cannot select the
# virtual display by accident.
for node in /dev/dri/renderD*; do
	if [ -e "$node" ]; then
		VIRTIO_RENDER=$node
		break
	fi
done
if [ -z "$VIRTIO_RENDER" ]; then
	echo "FAIL: virtio GPU did not expose a render node"
	echo FAILED > /host/status
	exit 1
fi
VIRTIO_FEATURES=""
for features in /sys/kernel/debug/dri/*/virtio-gpu-features; do
	if [ -r "$features" ]; then
		VIRTIO_FEATURES=$features
		break
	fi
done
if [ -z "$VIRTIO_FEATURES" ] ||
	! grep -Eq '^virgl[[:space:]]*:[[:space:]]*yes$' "$VIRTIO_FEATURES" ||
	! grep -Eq '^cap sets[[:space:]]*:[[:space:]]*[1-9][0-9]*$' "$VIRTIO_FEATURES"; then
	echo "FAIL: virtio GPU does not advertise virgl with a renderer capset"
	[ -n "$VIRTIO_FEATURES" ] && cat "$VIRTIO_FEATURES"
	echo FAILED > /host/status
	exit 1
fi
{
	printf 'virtio_render=%s\n' "$VIRTIO_RENDER"
	cat "$VIRTIO_FEATURES"
} >>"$RESULTS/system-info.log"
if ! timeout -k 2s 15s eglinfo -B -p gbm >"$RESULTS/virgl.log" 2>&1; then
	echo "FAIL: GBM/EGL probe failed on the virtio GPU"
	tail -80 "$RESULTS/virgl.log"
	echo FAILED > /host/status
	exit 1
fi
if ! grep -Eiq 'renderer.*virgl' "$RESULTS/virgl.log"; then
	echo "FAIL: virtio GPU is not using the accelerated virgl renderer"
	tail -80 "$RESULTS/virgl.log"
	echo FAILED > /host/status
	exit 1
fi

echo STARTED > /host/status
echo "=== kernel: $(uname -r) ==="
ls -l /dev/dri 2>/dev/null

DMESG_MARK="$(dmesg | wc -l)"
if ! insmod /opt/hermes-test/hermes_kms.ko initial_enabled=1 hotplug_events=1 \
	     initial_width=1280 initial_height=720 initial_refresh_hz=60; then
	echo "FAIL: could not load hermes_kms"
	echo FAILED > /host/status
	exit 1
fi
HERMES_LOADED=1
sleep 1

# Identify the hermes card and its render node
HERMES_CARD=""
for c in /sys/class/drm/card*; do
  if grep -q hermes "$c/device/uevent" 2>/dev/null; then
    HERMES_CARD="/dev/dri/$(basename "$c")"
    break
  fi
done
echo "HERMES_CARD=$HERMES_CARD"
[ -n "$HERMES_CARD" ] || {
  echo "FAIL: no Hermes-KMS card found"
  echo FAILED > /host/status
  exit 1
}
lsmod | grep -E 'hermes|virtio_gpu' || true

# Issue #22 regression: Mutter can render on the real GPU and PRIME-import the
# result into Hermes-KMS.  Re-exporting that pageless wrapper used to make the
# consumer fail with EGL_BAD_ALLOC.  Exercise the exact imported-scanout path
# before handing ownership of the output to the compositor.
if [ ! -c /dev/udmabuf ]; then
	modprobe udmabuf 2>>"$RESULTS/imported-scanout.log" || true
fi
if [ ! -c /dev/udmabuf ]; then
	echo "FAIL: /dev/udmabuf is unavailable" | tee -a "$RESULTS/imported-scanout.log"
	echo FAILED > /host/status
	exit 1
fi
if ! timeout -k 2s 20s stdbuf -oL -eL \
	/opt/hermes-test/hermes-imported-scanout-test \
	>"$RESULTS/imported-scanout.log" 2>&1; then
	echo "FAIL: imported-scanout regression test failed"
	tail -80 "$RESULTS/imported-scanout.log"
	echo FAILED > /host/status
	exit 1
fi
if ! grep -Fq 'RESULT: PASS - the imported buffer was handed back intact' \
	"$RESULTS/imported-scanout.log"; then
	echo "FAIL: imported-scanout test did not emit its success marker"
	echo FAILED > /host/status
	exit 1
fi
if ! grep -Eq '^PASS: small ARGB framebuffer [1-9][0-9]* accepted for cursor use$' \
	"$RESULTS/imported-scanout.log"; then
	echo "FAIL: small cursor-compatible ARGB framebuffer was not accepted"
	echo FAILED > /host/status
	exit 1
fi
sleep 1

# Enable the output so the compositor hotplugs it (owner fd keeps it connected)
/opt/hermes-test/hermes-kmsctl --device "$HERMES_CARD" --session-file "$SESSION_FILE" hold 1280x720@60 &
HOLD_PID=$!
for _ in $(seq 1 100); do
  [ -f "$SESSION_FILE" ] && break
  sleep 0.02
done
[ -f "$SESSION_FILE" ] || {
  echo "FAIL: output owner did not publish its session file"
  echo FAILED > /host/status
  exit 1
}
if [ "$(stat -c '%u %a' "$SESSION_FILE")" != "0 600" ]; then
	echo "FAIL: output owner published insecure session-file credentials"
	echo FAILED > /host/status
	exit 1
fi

# Create/activate a logind session so mutter can find a matching session.
export XDG_SEAT=seat0
# The session leader must be a long-lived process (this script), so pass $$.
if ! SID=$(/opt/hermes-test/mksession $$ seat0 2>>"$RESULTS/mksession.log") ||
	[ -z "$SID" ]; then
	echo "FAIL: could not create the logind Wayland session"
	echo FAILED > /host/status
	exit 1
fi
export XDG_SESSION_ID="$SID"
if ! loginctl activate "$SID" 2>>"$RESULTS/mksession.log" ||
	! loginctl show-session "$SID" -p State -p Type -p Seat -p Active \
		>>"$RESULTS/mksession.log" 2>&1; then
	echo "FAIL: could not activate or inspect the logind session"
	echo FAILED > /host/status
	exit 1
fi
echo "SESSION_ID=$SID" >> "$RESULTS/mksession.log"
loginctl list-seats >> "$RESULTS/mksession.log" 2>&1 || true
if ! grep -Fxq 'Seat=seat0' "$RESULTS/mksession.log" ||
	! grep -Fxq 'Type=wayland' "$RESULTS/mksession.log" ||
	! grep -Fxq 'Active=yes' "$RESULTS/mksession.log" ||
	! grep -Fxq 'State=active' "$RESULTS/mksession.log"; then
	echo "FAIL: logind session is not active Wayland on seat0"
	echo FAILED > /host/status
	exit 1
fi

# Start the compositor session on the virtio-gpu.
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/0/bus"
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --fork 2>/dev/null || true

# Force Mutter's zero-copy multi-GPU path: this calls gbm_bo_import() for the
# framebuffer rendered on virgl and scanned out on Hermes-KMS, which is the
# real compositor path behind issue #22.
export MUTTER_DEBUG=kms,render,backend
export MUTTER_DEBUG_MULTI_GPU_FORCE_COPY_MODE=zero-copy
gnome-shell --wayland --no-x11 >"$RESULTS/gnome-shell.log" 2>&1 &
COMPO_PID=$!

sleep 8
echo "=== compositor alive: $(kill -0 "$COMPO_PID" 2>/dev/null && echo yes || echo no) ==="
if ! kill -0 "$COMPO_PID" 2>/dev/null; then
	echo "FAIL: GNOME Shell/Mutter did not remain alive"
	tail -120 "$RESULTS/gnome-shell.log"
	echo FAILED > /host/status
	exit 1
fi

# Re-import a real Mutter frame through EGL/GBM on its accelerated primary
# render node. virtio/virgl does not guarantee that the guest CPU mapping is a
# useful reference for the host-side resource, so the deterministic udmabuf
# test above proves buffer identity and this check focuses on the GPU path.
# CUDA is intentionally absent; exit 2 means only optional stages were skipped.
timeout -k 2s 45s /opt/hermes-test/hermes-egl-import-check \
	--session-file "$SESSION_FILE" --gpu "$VIRTIO_RENDER" \
	--wait-ms 5000 --no-cpu-ref --no-cuda \
	>"$RESULTS/egl-import.log" 2>&1
EGL_IMPORT_RC=$?
EGL_STAGE_PASSES=$(grep -Ec '^  (frame acquisition|fence wait|EGL import|OpenGL validation):[[:space:]]+PASS$' \
	"$RESULTS/egl-import.log" || true)
if [ "$EGL_IMPORT_RC" -ne 2 ] || [ "$EGL_STAGE_PASSES" -ne 4 ] ||
	! grep -Fq 'PASS: eglCreateImage imported the Hermes DMA-BUF' \
		"$RESULTS/egl-import.log" ||
	! grep -Fq 'PASS: GPU readback of imported frame' \
		"$RESULTS/egl-import.log" ||
	! grep -Fq 'PASS: GL converter pass (imported -> own texture)' \
		"$RESULTS/egl-import.log" ||
	grep -Eq 'EGL_BAD_ALLOC|^FAIL:' "$RESULTS/egl-import.log"; then
	echo "FAIL: EGL re-import of the live Mutter frame failed"
	tail -120 "$RESULTS/egl-import.log"
	echo FAILED > /host/status
	exit 1
fi

ydotoold >"$RESULTS/ydotoold.log" 2>&1 &
YDOTOOLD_PID=$!
sleep 1

# Start the cursor-plane probe (unbuffered), then sweep the pointer across the
# whole desktop (covers the hermes output wherever mutter placed it).
/opt/hermes-test/hermes_cursor_probe --device "$HERMES_CARD" \
  --session-file "$SESSION_FILE" --interval-ms 100 \
  >"$RESULTS/cursor-probe.log" 2>&1 &
PROBE_PID=$!

# Pixel peek over the WHOLE captured framebuffer (no --region): any pointer
# pixels drawn into the primary FB change the checksum, even between new
# frames (see the [poll] lines).  Leave it running for the complete sweep and
# let the session credential select the matching render node.
/opt/hermes-test/hermes_pixel_peek --session-file "$SESSION_FILE" \
  >"$RESULTS/pixel-peek.log" 2>&1 &
PEEK_PID=$!
sleep 1

MOTION_FAILURES=0
for pass in 1 2; do
  for x in 200 400 600 800 1000 1200 1400 1600 1800 2000 2200; do
    ydotool mousemove --absolute -- "$x" 360 2>>"$RESULTS/ydotool.log" || MOTION_FAILURES=$((MOTION_FAILURES + 1))
    sleep 0.35
  done
done

sleep 3
FAILURES=0
if [ "$MOTION_FAILURES" -ne 0 ]; then
  echo "FAIL: $MOTION_FAILURES pointer motions failed"
	FAILURES=$((FAILURES + 1))
fi
for process in "compositor:$COMPO_PID" "owner:$HOLD_PID" "cursor-probe:$PROBE_PID" \
	       "pixel-peek:$PEEK_PID" "ydotoold:$YDOTOOLD_PID"; do
	name=${process%%:*}
	pid=${process#*:}
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "FAIL: $name exited before the test completed"
		FAILURES=$((FAILURES + 1))
	fi
done
if ! cleanup; then
	echo "FAIL: hermes_kms did not unload cleanly"
	FAILURES=$((FAILURES + 1))
fi
sleep 1
if ! grep -Eq 'cursor_fb=[1-9][0-9]* cursor_crtc=[1-9][0-9]*' \
	"$RESULTS/cursor-probe.log"; then
	echo "FAIL: compositor never attached a framebuffer to the cursor plane"
	FAILURES=$((FAILURES + 1))
fi
CURSOR_POSITIONS=$(grep -E 'cursor_fb=[1-9][0-9]* cursor_crtc=[1-9][0-9]*' \
	"$RESULTS/cursor-probe.log" | \
	sed -n 's/.*cursor_crtc_pos=(\([^)]*\)).*/\1/p' | sort -u | wc -l)
if [ "$CURSOR_POSITIONS" -lt 2 ]; then
	echo "FAIL: cursor CRTC position did not change (unique=$CURSOR_POSITIONS)"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Eq '^\[uapi\].*cursor_ready=1.*visible=1' \
	"$RESULTS/cursor-probe.log"; then
	echo "FAIL: WAIT_UPDATE did not report a visible cursor update"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Eq '^\[uapi\].*visible=1 position_valid=1 hotspot_valid=0 geometry_valid=1 buffer_valid=1.*clipped=\(-?[0-9]+,-?[0-9]+ [1-9][0-9]*x[1-9][0-9]*\).*alpha_pixels=[1-9][0-9]*$' \
	"$RESULTS/cursor-probe.log"; then
	echo "FAIL: cursor UAPI did not expose valid visible geometry and alpha pixels"
	FAILURES=$((FAILURES + 1))
fi
UAPI_CURSOR_POSITIONS=$(grep -E '^\[uapi\].*visible=1' \
	"$RESULTS/cursor-probe.log" | \
	sed -n 's/.* position=(\([^)]*\)).*/\1/p' | sort -u | wc -l)
if [ "$UAPI_CURSOR_POSITIONS" -lt 2 ]; then
	echo "FAIL: cursor UAPI position did not change (unique=$UAPI_CURSOR_POSITIONS)"
	FAILURES=$((FAILURES + 1))
fi
if grep -Eq 'WAIT_UPDATE|ACQUIRE_CURSOR|cursor buffer validation|malformed metadata|incoherent flags' \
	"$RESULTS/cursor-probe.log"; then
	echo "FAIL: cursor UAPI probe reported an error"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Eq '^driver: hermes-kms [0-9]+\.[0-9]+\.[0-9]+ uapi=11$' \
	"$RESULTS/pixel-peek.log"; then
	echo "FAIL: capture probe did not exercise Hermes-KMS UAPI 11"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Fq 'Running GNOME Shell (using mutter ' \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: GNOME Shell/Mutter startup marker is missing"
	FAILURES=$((FAILURES + 1))
fi
VIRTIO_CARD=$(sed -n "s/.*Added device '\([^']*\)' (virtio_gpu).*/\1/p" \
	"$RESULTS/gnome-shell.log" | head -1)
if [ -z "$VIRTIO_CARD" ]; then
	echo "FAIL: Mutter did not add the accelerated virtio GPU"
	FAILURES=$((FAILURES + 1))
elif ! grep -Fq "GPU $VIRTIO_CARD selected as primary" \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: Mutter did not select the virtio GPU as primary"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Fq "Added device '$HERMES_CARD' (hermes-kms)" \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: Mutter did not add the Hermes-KMS device"
	FAILURES=$((FAILURES + 1))
fi
HERMES_CURSOR_PLANE=$(sed -n "s|.*Adding cursor plane \([0-9][0-9]*\) ($HERMES_CARD).*|\1|p" \
	"$RESULTS/gnome-shell.log" | head -1)
if [ -z "$HERMES_CURSOR_PLANE" ]; then
	echo "FAIL: Mutter did not enumerate the Hermes cursor plane"
	FAILURES=$((FAILURES + 1))
elif ! grep -Eq "Plane $HERMES_CURSOR_PLANE is .*can be used as cursor for CRTC" \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: Mutter did not accept the Hermes plane for cursor use"
	FAILURES=$((FAILURES + 1))
fi
if ! grep -Fq "Using zero-copy for $HERMES_CARD succeeded once." \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: Mutter did not prove a successful real PRIME import into Hermes-KMS"
	FAILURES=$((FAILURES + 1))
fi
if grep -Eqi "Zero-copy disabled for $HERMES_CARD, import failed|EGL_BAD_ALLOC|Page flip failed|drmModeAddFB failed" \
	"$RESULTS/gnome-shell.log"; then
	echo "FAIL: GNOME Shell/Mutter reported a rendering or PRIME-import regression"
	FAILURES=$((FAILURES + 1))
fi
PIXEL_SAMPLES=$(grep -Ec 'checksum=0x[[:xdigit:]]{8}' \
	"$RESULTS/pixel-peek.log" || true)
PIXEL_CHECKSUMS=$(sed -n 's/.*checksum=\(0x[[:xdigit:]]\{8\}\).*/\1/p' \
	"$RESULTS/pixel-peek.log" | sort -u | wc -l)
echo "pixel samples=$PIXEL_SAMPLES unique_checksums=$PIXEL_CHECKSUMS"
if [ "$PIXEL_SAMPLES" -lt 4 ]; then
	echo "FAIL: capture probe produced only $PIXEL_SAMPLES checksum samples"
	FAILURES=$((FAILURES + 1))
fi
if grep -Eq 'SESSION_ACCESS BIND failed|could not find the render node|WAIT_FRAME:|ACQUIRE_FRAME:|producer fence (timed out|poll failed|returned|wait failed)|DMA_BUF_IOCTL_SYNC|mmap dmabuf:|unsupported (DMA-BUF|pixel format)|frame metadata exceeds|mapping length overflow' \
  "$RESULTS/pixel-peek.log"; then
	echo "FAIL: capture probe reported an error"
	FAILURES=$((FAILURES + 1))
fi

SPLAT=$(dmesg | tail -n +"$((DMESG_MARK + 1))" | grep -iE \
	'BUG:|KASAN|use-after-free|WARNING:|RIP:|slab corruption|Redzone|Poison|refcount|general protection|null pointer' \
	|| true)
if [ -n "$SPLAT" ]; then
	printf '%s\n' "$SPLAT"
	echo "FAIL: kernel splat detected"
	FAILURES=$((FAILURES + 1))
fi

echo "=== journal (hermes + mutter) ==="
journalctl -k --no-pager 2>/dev/null | grep -iE 'hermes' | tail -30
journalctl --no-pager 2>/dev/null | grep -iE 'mutter|gnome-shell' | tail -20

if [ "$FAILURES" -ne 0 ]; then
  echo FAILED > /host/status
  echo "=== FAILED ($FAILURES assertions) ==="
  exit 1
fi

echo DONE > /host/status
echo "=== PASS ==="
ls -la "$RESULTS"

exit 0
