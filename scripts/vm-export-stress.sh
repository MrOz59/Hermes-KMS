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
BIN="${TMPDIR:-/tmp}/hermes-export-stress"
SECONDS_RUN="${1:-15}"
THREADS="${2:-12}"
FAIL=0

if [ "$(id -u)" -ne 0 ]; then echo "run as root: sudo $0" >&2; exit 1; fi
if [ ! -f "$KO" ]; then echo "module not built: $KO" >&2; exit 1; fi

if ! cc -O2 -pthread -I/usr/include/libdrm -I"$REPO/include/uapi" \
        -o "$BIN" "$SRC" -ldrm 2>/tmp/stress-build.log; then
  echo "### could not build hermes-export-stress:"; cat /tmp/stress-build.log; exit 1
fi

# Report whether a memory-debug facility is active so results are interpretable.
echo "### slub_debug: $(cat /sys/kernel/slab/dma-buf*/red_zone 2>/dev/null | head -1 || echo '?')"
grep -qE "slub_debug|page_poison|kasan" /proc/cmdline \
  && echo "### mem-debug cmdline: $(cat /proc/cmdline)" \
  || echo "### WARNING: no slub_debug/page_poison/kasan on cmdline -- UAF coverage is weak. See script header."

lsmod | grep -q '^hermes_kms' && rmmod hermes_kms 2>/dev/null
insmod "$KO" initial_enabled=1 hotplug_events=0 \
       initial_width=1920 initial_height=1080 initial_refresh_hz=60 || {
  echo "### insmod FAILED"; exit 1; }
sleep 0.3

# Snapshot dmesg high-water so we only judge splats produced during the run.
DMESG_MARK="$(dmesg | wc -l)"

echo "### running stress (${SECONDS_RUN}s, ${THREADS} threads)"
"$BIN" "$SECONDS_RUN" "$THREADS"
STRESS_RC=$?

echo "### dmesg splat check (new lines only):"
SPLAT=$(dmesg | tail -n +"$((DMESG_MARK + 1))" \
  | grep -iE "BUG:|KASAN|use-after-free|WARNING:|RIP:|slab corruption|Redzone|Poison|refcount|general protection|null pointer" \
  || true)
if [ -n "$SPLAT" ]; then
  echo "$SPLAT" | sed 's/^/###   /'; FAIL=1
else
  echo "###   clean"
fi

# Verify the export cache fully drained: after the producer stops, disabling the
# output (rmmod path) must drop all cached dma-bufs with no leak warning.
rmmod hermes_kms 2>/tmp/rmmod.log && echo "### rmmod OK" \
  || { echo "### rmmod FAILED:"; cat /tmp/rmmod.log; FAIL=1; }

[ "$STRESS_RC" -ne 0 ] && FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
  echo "### EXPORT STRESS PASSED (no splats, stress PASS, clean rmmod)"
else
  echo "### EXPORT STRESS FAILED"
fi
exit "$FAIL"
