#!/usr/bin/env bash
# Automated pacing / page-flip self-test for Hermes-KMS.
#
# Validates that the software vblank timer fires at the mode's exact refresh
# with NO missed vblanks (no dropped page-flip slots). It uses a small DRM
# helper (hermes-vblank-meter) that holds vblank enabled and times N vblanks
# via DRM_IOCTL_WAIT_VBLANK -- the same vblank-event mechanism the compositor
# relies on -- and cross-checks the driver's own vblank_overrun_count from
# debugfs. No GPU, no compositor, no tty: runs headless in the virtme-ng VM or
# on the host.
#
# Run: sudo ./scripts/vm-pacing-test.sh
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
METER_SRC="$REPO/scripts/hermes-vblank-meter.c"
METER_BIN="${TMPDIR:-/tmp}/hermes-vblank-meter"
WINDOW_VBLANKS=240   # number of vblanks to time per rate (4s @60, ~1.7s @144)
RATES="60 120 144"
FAIL=0

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo $0" >&2; exit 1
fi
if [ ! -f "$KO" ]; then
  echo "module not built: $KO" >&2; exit 1
fi

# Build the meter once (gcc/cc + libdrm headers).
if ! cc -O2 -I/usr/include/libdrm -o "$METER_BIN" "$METER_SRC" -ldrm 2>/tmp/meter-build.log; then
  echo "### could not build hermes-vblank-meter:"; cat /tmp/meter-build.log; exit 1
fi

find_stats() {
  local f
  for f in /sys/kernel/debug/dri/*/hermes_kms_stats; do
    [ -e "$f" ] && { echo "$f"; return 0; }
  done
  return 1
}
read_counter() {  # $1=name $2=file
  awk -v k="$1:" '$0 ~ "^"k {print $2; exit}' "$2" 2>/dev/null
}
cleanup() {
  lsmod | grep -q '^hermes_kms' && rmmod hermes_kms 2>/dev/null
}
trap cleanup EXIT

[ -d /sys/kernel/debug/dri ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null

for hz in $RATES; do
  echo "### ===== target ${hz}Hz ====="
  cleanup; sleep 0.3

  insmod "$KO" initial_enabled=1 hotplug_events=0 \
         initial_width=1920 initial_height=1080 initial_refresh_hz="$hz" || {
    echo "### insmod FAILED"; FAIL=1; continue; }
  sleep 0.3

  STATS="$(find_stats)" || { echo "### no debugfs stats (CONFIG_DEBUG_FS off?)"; FAIL=1; continue; }
  ov0=$(read_counter vblank_overrun_count "$STATS")

  OUT="$("$METER_BIN" "$WINDOW_VBLANKS" 2>&1)"
  meter_rc=$?
  echo "$OUT" | sed 's/^/###   /'

  ov1=$(read_counter vblank_overrun_count "$STATS")
  overruns=$(( ${ov1:-0} - ${ov0:-0} ))
  echo "###   driver vblank_overrun_count delta: +$overruns"

  verdict="PASS"
  [ "$meter_rc" -ne 0 ] && verdict="FAIL(meter)"
  [ "${overruns:-0}" -ne 0 ] && verdict="FAIL(overruns)"
  echo "###   -> $verdict"
  [ "$verdict" = "PASS" ] || FAIL=1

  echo "### dmesg splat check:"
  dmesg | grep -iE "BUG:|WARNING:|RIP:|sleeping|irqs disabled|circular" | tail -3 \
    || echo "###   clean"
done

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### PACING TEST PASSED (all rates within 5%, zero missed vblanks)"
else
  echo "### PACING TEST FAILED"
fi
exit "$FAIL"
