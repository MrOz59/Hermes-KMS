#!/usr/bin/env bash
# Concurrent dma-buf / fence lifetime stress for Hermes-KMS (hardening item #4).
#
# Runs hermes-export-stress: many threads hammer ACQUIRE_FRAME (dma-buf +
# sync_file export) while a producer churns the scanout framebuffer, so the
# export cache is dropped/replaced under in-flight exporters. Pairs best with a
# memory-debug kernel:
#
#   cd ~/linux-debug
#   vng --append "slub_debug=FZPU page_poison=1" --run . -- \
#       sudo /path/to/scripts/vm-export-stress.sh
#
# (Or rebuild the VM kernel with CONFIG_KASAN=y for the strongest UAF coverage.)
# slub_debug=FZPU gives redzone + free-poison + sanity + user-tracking, which
# catches use-after-free / overflow on the dma_buf and fence allocations the
# export path touches. PASS == stress prints PASS *and* dmesg is clean.
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
SRC="$REPO/scripts/hermes-export-stress.c"
TEST_TMP="$(mktemp -d /tmp/hermes-export-stress.XXXXXX)"
BIN="$TEST_TMP/hermes-export-stress"
BUILD_LOG="$TEST_TMP/build.log"
RMMOD_LOG="$TEST_TMP/rmmod.log"
SECONDS_RUN="${1:-15}"
THREADS="${2:-12}"
FAIL=0
LOADED_BY_TEST=0

cleanup() {
  if [ "$LOADED_BY_TEST" -eq 1 ]; then
    rmmod hermes_kms 2>/dev/null || true
  fi
  rm -rf -- "$TEST_TMP"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "$(id -u)" -ne 0 ]; then echo "run as root: sudo $0" >&2; exit 1; fi
if [ ! -f "$KO" ]; then echo "module not built: $KO" >&2; exit 1; fi
if [ -d /sys/module/hermes_kms ]; then
  echo "hermes_kms is already loaded; use a disposable VM or unload it first" >&2
  exit 1
fi

if ! cc -O2 -pthread -I/usr/include/libdrm -I"$REPO/include/uapi" \
        -o "$BIN" "$SRC" -ldrm 2>"$BUILD_LOG"; then
  echo "### could not build hermes-export-stress:"; cat "$BUILD_LOG"; exit 1
fi

# Report whether a memory-debug facility is active so results are interpretable.
echo "### slub_debug: $(cat /sys/kernel/slab/dma-buf*/red_zone 2>/dev/null | head -1 || echo '?')"
grep -qE "slub_debug|page_poison|kasan" /proc/cmdline \
  && echo "### mem-debug cmdline: $(cat /proc/cmdline)" \
  || echo "### WARNING: no slub_debug/page_poison/kasan on cmdline -- UAF coverage is weak. See script header."

insmod "$KO" initial_enabled=1 hotplug_events=0 \
       initial_width=1920 initial_height=1080 initial_refresh_hz=60 || {
  echo "### insmod FAILED"; exit 1; }
LOADED_BY_TEST=1
sleep 0.3

# Snapshot dmesg high-water so we only judge splats produced during the run.
DMESG_MARK="$(dmesg | wc -l)"

echo "### running stress (${SECONDS_RUN}s, ${THREADS} threads)"
"$BIN" "$SECONDS_RUN" "$THREADS"
STRESS_RC=$?

# Verify the export cache fully drained: after the producer stops, disabling the
# output (rmmod path) must drop all cached dma-bufs with no leak warning.
rmmod hermes_kms 2>"$RMMOD_LOG" && { echo "### rmmod OK"; LOADED_BY_TEST=0; } \
  || { echo "### rmmod FAILED:"; cat "$RMMOD_LOG"; FAIL=1; }

echo "### dmesg splat check (new lines, including unload):"
SPLAT=$(dmesg | tail -n +"$((DMESG_MARK + 1))" \
  | grep -iE "BUG:|KASAN|use-after-free|WARNING:|RIP:|slab corruption|Redzone|Poison|refcount|general protection|null pointer" \
  || true)
if [ -n "$SPLAT" ]; then
  echo "$SPLAT" | sed 's/^/###   /'; FAIL=1
else
  echo "###   clean"
fi

[ "$STRESS_RC" -ne 0 ] && FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### EXPORT STRESS PASSED (no splats, stress PASS, clean rmmod)"
else
  echo "### EXPORT STRESS FAILED"
fi
exit "$FAIL"
