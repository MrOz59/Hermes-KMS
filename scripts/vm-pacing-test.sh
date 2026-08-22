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
TEST_TMP="$(mktemp -d /tmp/hermes-pacing.XXXXXX)"
METER_BIN="$TEST_TMP/hermes-vblank-meter"
BUILD_LOG="$TEST_TMP/meter-build.log"
WINDOW_VBLANKS=240   # number of vblanks to time per rate (4s @60, ~1.7s @144)
RATES="60 120 144"
FAIL=0
LOADED_BY_TEST=0
MOUNTED_DEBUGFS=0

cleanup() {
  if [ "$LOADED_BY_TEST" -eq 1 ]; then
	if ! rmmod hermes_kms; then
		return 1
	fi
	LOADED_BY_TEST=0
  fi
	return 0
}
final_cleanup() {
  cleanup
  if [ "$MOUNTED_DEBUGFS" -eq 1 ]; then
    umount /sys/kernel/debug 2>/dev/null || true
  fi
  rm -rf -- "$TEST_TMP"
}
trap final_cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo $0" >&2; exit 1
fi
if [ ! -f "$KO" ]; then
  echo "module not built: $KO" >&2; exit 1
fi
if [ -d /sys/module/hermes_kms ]; then
  echo "hermes_kms is already loaded; use a disposable VM or unload it first" >&2
  exit 1
fi

# Build the meter once (gcc/cc + libdrm headers).
if ! cc -O2 -I/usr/include/libdrm -o "$METER_BIN" "$METER_SRC" -ldrm 2>"$BUILD_LOG"; then
  echo "### could not build hermes-vblank-meter:"; cat "$BUILD_LOG"; exit 1
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
if [ ! -d /sys/kernel/debug/dri ] &&
   mount -t debugfs none /sys/kernel/debug 2>/dev/null; then
  MOUNTED_DEBUGFS=1
fi

for hz in $RATES; do
	echo "### ===== target ${hz}Hz ====="
	if ! cleanup; then
		echo "### previous module instance did not unload"; FAIL=1; break
	fi
	sleep 0.3
	DMESG_MARK="$(dmesg | wc -l)"

  insmod "$KO" initial_enabled=1 hotplug_events=0 \
         initial_width=1920 initial_height=1080 initial_refresh_hz="$hz" || {
    echo "### insmod FAILED"; FAIL=1; continue; }
  LOADED_BY_TEST=1
  sleep 0.3

  STATS="$(find_stats)" || { echo "### no debugfs stats (CONFIG_DEBUG_FS off?)"; FAIL=1; continue; }
  ov0=$(read_counter vblank_overrun_count "$STATS")

  OUT="$("$METER_BIN" "$WINDOW_VBLANKS" 2>&1)"
  meter_rc=$?
  echo "$OUT" | sed 's/^/###   /'

	  ov1=$(read_counter vblank_overrun_count "$STATS")
  overruns=$(( ${ov1:-0} - ${ov0:-0} ))
	  echo "###   driver vblank_overrun_count delta: +$overruns"
	rmmod_failed=0
	if ! cleanup; then
		rmmod_failed=1
	fi
	SPLAT="$(dmesg | tail -n +"$((DMESG_MARK + 1))" \
	  | grep -iE 'BUG:|KASAN|use-after-free|WARNING:|RIP:|slab corruption|Redzone|Poison|refcount|general protection|null pointer|sleeping|irqs disabled|circular' \
	  || true)"

  verdict="PASS"
	  [ "$meter_rc" -ne 0 ] && verdict="FAIL(meter)"
	  [ "${overruns:-0}" -ne 0 ] && verdict="FAIL(overruns)"
	[ "$rmmod_failed" -ne 0 ] && verdict="FAIL(rmmod)"
	[ -n "$SPLAT" ] && verdict="FAIL(dmesg)"
  echo "###   -> $verdict"
  [ "$verdict" = "PASS" ] || FAIL=1

	echo "### dmesg splat check (new lines only):"
	if [ -n "$SPLAT" ]; then
		printf '%s\n' "$SPLAT" | tail -20 | sed 's/^/###   /'
	else
		echo "###   clean"
	fi
done

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### PACING TEST PASSED (all rates within 5%, zero missed vblanks)"
else
  echo "### PACING TEST FAILED"
fi
exit "$FAIL"
