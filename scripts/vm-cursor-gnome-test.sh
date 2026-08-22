#!/bin/bash
# Cursor/plane test: GNOME/Mutter driving hermes-kms inside a virtme-ng VM.
#
# Loads the host-built hermes_kms.ko, starts a GNOME/Mutter session on the
# virtio-gpu, enables the virtual output, moves the pointer with ydotool and
# records (a) whether Mutter uses the cursor plane, (b) whether pointer pixels
# appear in the captured primary framebuffer.
#
# Run inside the VM as root (copied to /opt/hermes-test/run-gnome-test.sh by the host).
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH
RESULTS=/host/results
mkdir -p "$RESULTS"
exec >"$RESULTS/test.log" 2>&1
set -x

systemctl start dbus 2>>/tmp/svc.log || true
/usr/lib/systemd/systemd-logind >"$RESULTS/logind.log" 2>&1 &
LOGIND_PID=$!
sleep 2
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
modprobe drm 2>/dev/null || true
insmod /opt/hermes-test/uinput.ko 2>/dev/null || true
insmod /opt/hermes-test/virtio_dma_buf.ko 2>/dev/null || true
insmod /opt/hermes-test/virtio-gpu.ko 2>/dev/null || true
sleep 1

echo STARTED > /host/status
echo "=== kernel: $(uname -r) ==="
ls -l /dev/dri 2>/dev/null

insmod /opt/hermes-test/hermes_kms.ko initial_enabled=1 hotplug_events=1 \
      initial_width=1280 initial_height=720 initial_refresh_hz=60
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
lsmod | grep -E 'hermes|virtio_gpu' || true

# Enable the output so the compositor hotplugs it (owner fd keeps it connected)
/opt/hermes-test/hermes-kmsctl --device "$HERMES_CARD" hold 1280x720@60 &
HOLD_PID=$!
sleep 2

# Create/activate a logind session so mutter can find a matching session.
export XDG_SEAT=seat0
# The session leader must be a long-lived process (this script), so pass $$.
SID=$(/opt/hermes-test/mksession $$ seat0 2>>"$RESULTS/mksession.log" || true)
if [ -n "$SID" ]; then
  export XDG_SESSION_ID="$SID"
  loginctl activate "$SID" 2>>"$RESULTS/mksession.log" || true
  loginctl show-session "$SID" -p State -p Type -p Seat -p Active >> "$RESULTS/mksession.log" 2>&1 || true
fi
echo "SESSION_ID=${SID:-none}" >> "$RESULTS/mksession.log"
loginctl list-seats >> "$RESULTS/mksession.log" 2>&1 || true

# Start the compositor session on the virtio-gpu.
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/0/bus"
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --fork 2>/dev/null || true

# Try gnome-shell first; mutter standalone is the fallback (same KMS code).
if command -v gnome-shell >/dev/null; then
  gnome-shell --wayland --no-x11 >"$RESULTS/gnome-shell.log" 2>&1 &
  COMPO_PID=$!
else
  mutter --wayland >"$RESULTS/mutter.log" 2>&1 &
  COMPO_PID=$!
fi

sleep 8
# If gnome-shell failed, retry with plain mutter.
if ! kill -0 $COMPO_PID 2>/dev/null; then
  echo "=== gnome-shell died; falling back to mutter ==="
  mutter --wayland >"$RESULTS/mutter.log" 2>&1 &
  COMPO_PID=$!
  sleep 6
fi

echo "=== compositor alive: $(kill -0 $COMPO_PID 2>/dev/null && echo yes || echo no) ==="

ydotoold >"$RESULTS/ydotoold.log" 2>&1 &
YDOTOOLD_PID=$!
sleep 1

# Start the cursor-plane probe (unbuffered), then sweep the pointer across the
# whole desktop (covers the hermes output wherever mutter placed it).
/opt/hermes-test/hermes_cursor_probe --device "$HERMES_CARD" --interval-ms 100 \
  >"$RESULTS/cursor-probe.log" 2>&1 &
PROBE_PID=$!

# Pixel peek over the WHOLE captured framebuffer (no --region): any pointer
# pixels drawn into the primary FB change the checksum, even between new
# frames (see the [poll] lines).
HERMES_RENDER=$(ls /dev/dri/by-path/ 2>/dev/null | grep -m1 'hermes.*render' | sed 's|^|/dev/dri/by-path/|')
[ -n "$HERMES_RENDER" ] || HERMES_RENDER=/dev/dri/renderD129
/opt/hermes-test/hermes_pixel_peek --device "$HERMES_RENDER" --frames 4 \
  >"$RESULTS/pixel-peek.log" 2>&1 &
PEEK_PID=$!
sleep 1

for pass in 1 2; do
  for x in 200 400 600 800 1000 1200 1400 1600 1800 2000 2200; do
    ydotool mousemove "$x" 360 2>>"$RESULTS/ydotool.log"
    sleep 0.35
  done
done

sleep 3
kill $PEEK_PID 2>/dev/null
kill $YDOTOOLD_PID 2>/dev/null
kill $LOGIND_PID 2>/dev/null
kill $PROBE_PID 2>/dev/null
kill $HOLD_PID 2>/dev/null
kill $COMPO_PID 2>/dev/null
sleep 1

echo "=== journal (hermes + mutter) ==="
journalctl -k --no-pager 2>/dev/null | grep -iE 'hermes' | tail -30
journalctl --no-pager 2>/dev/null | grep -iE 'mutter|gnome-shell' | tail -20

echo DONE > /host/status
echo "=== DONE ==="
ls -la "$RESULTS"

exit 0
