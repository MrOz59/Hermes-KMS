#!/usr/bin/env bash
# Drive Hermes-KMS with a real Wayland compositor (Weston, software/pixman
# renderer) inside the GPU-less virtme-ng VM, and measure how many frames the
# compositor scans out per second. This is the real test of the vblank timer:
# does the compositor compose at the mode's refresh (60Hz)?
#
# Weston --renderer=pixman composes on the CPU, so no GPU is needed.
set -u
REPO=/home/ozzy/Projects/Hermes-KMS
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"

echo "### kernel: $(uname -r)"
insmod "$KO" initial_enabled=1 || { echo "insmod FAILED"; exit 1; }
sleep 0.5
HERMES_CARD=$(readlink -f "$(ls /dev/dri/by-path/*hermes*card 2>/dev/null | head -1)")
echo "### hermes card: $HERMES_CARD"

export XDG_RUNTIME_DIR=/tmp/xdg-weston
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

# In the bare VM there is no seatd/logind session. Start seatd ourselves (we are
# root) so libseat can open DRM/input. Weston then connects to it.
seatd -g video >/tmp/seatd.log 2>&1 &
SEATD_PID=$!
sleep 1
export LIBSEAT_BACKEND=seatd
export XDG_SEAT=seat0

# Weston needs a seat; in the bare VM run it with the launcher disabled and the
# DRM device pointed at Hermes-KMS, CPU renderer.
echo "### starting weston (drm backend, pixman renderer) for ~10s"
timeout 14 weston \
  --backend=drm \
  --drm-device="$(basename "$HERMES_CARD")" \
  --renderer=pixman \
  --idle-time=0 \
  --continue-without-input >/tmp/weston-out.log 2>&1 &
WPID=$!

# Wait for weston to come up, then launch animating clients so the compositor
# repaints every frame (a static desktop only composes on change).
sleep 3
echo "### sockets in XDG_RUNTIME_DIR:"; ls -la "$XDG_RUNTIME_DIR"/ 2>/dev/null | grep wayland
WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v lock | head -1 | xargs -r basename)
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
echo "### WAYLAND_DISPLAY=$WAYLAND_DISPLAY — launching weston-flower + simple-shm"
weston-flower >/tmp/client1.log 2>&1 &
weston-simple-shm >/tmp/client2.log 2>&1 &
sleep 1
echo "### client1 (flower) log:"; head -5 /tmp/client1.log 2>/dev/null
echo "### client2 (shm) log:"; head -5 /tmp/client2.log 2>/dev/null

# Sample the driver's frame_sequence to get the real compositor scanout FPS
# while the client is animating.
sleep 1
S1=$(dmesg | grep -oE "sequence=[0-9]+" | tail -1 | cut -d= -f2)
T1=$(date +%s.%N)
sleep 4
S2=$(dmesg | grep -oE "sequence=[0-9]+" | tail -1 | cut -d= -f2)
T2=$(date +%s.%N)
if [ -n "${S1:-}" ] && [ -n "${S2:-}" ]; then
  awk "BEGIN{d=$S2-$S1; t=$T2-$T1; if(t>0) printf \"### MEASURED COMPOSITOR FPS: ~%.1f (frames %d->%d over %.1fs)\n\", d/t, $S1, $S2, t; else print \"### no time delta\"}"
else
  echo "### could not sample frame_sequence (S1=${S1:-} S2=${S2:-})"
fi

kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; sleep 1

echo "### weston output (key lines):"
grep -iE "backend|drm|output|pixman|repaint|error|fail|hermes|virtual" /tmp/weston-out.log 2>/dev/null | head -20
echo "### dmesg (driver side):"
dmesg | grep -iE "enabled virtual display|first active scanout|BUG:|WARNING:|sleeping|irqs disabled" | tail -8

for try in 1 2 3; do
  rmmod hermes_kms 2>/dev/null && { echo "### rmmod OK"; break; }
  sleep 2
done
