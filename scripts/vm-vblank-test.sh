#!/usr/bin/env bash
# Run INSIDE the virtme-ng VM (booted with the debug kernel) to exercise the
# Hermes-KMS vblank timer under lockdep/atomic-sleep, with modetest driving the
# display (no compositor). Surfaces the soft-lock / lock-ordering / refcount bugs
# that hung the host, without risking the real desktop.
#
# Usage on the host:
#   make modules KDIR=~/linux-debug         # build the module for the VM kernel
#   cd ~/linux-debug
#   vng --run -- /home/ozzy/Projects/Hermes-KMS/scripts/vm-vblank-test.sh
set -u

REPO=/home/ozzy/Projects/Hermes-KMS
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"

echo "### kernel: $(uname -r)"
echo "### lockdep active? $(grep -c PROVE_LOCKING /proc/config.gz 2>/dev/null || echo '?')"

echo "### loading hermes_kms (isolated, vblank exercised by modetest)"
insmod "$KO" initial_enabled=1 hotplug_events=0 initial_width=1920 \
       initial_height=1080 initial_refresh_hz=60 || { echo "insmod FAILED"; }

sleep 1
CARD=$(ls /dev/dri/by-path/*hermes*card 2>/dev/null | head -1)
CARD=$(readlink -f "$CARD" 2>/dev/null)
echo "### hermes card: ${CARD:-not found}"

# Discover connector/crtc/plane and drive a modeset so the vblank timer runs.
if command -v modetest >/dev/null 2>&1 && [ -n "$CARD" ]; then
  echo "### modetest connectors:"
  modetest -M hermes-kms -c 2>/dev/null | grep -iE "^[0-9]+.*connected|Virtual" | head
  CONN=$(modetest -M hermes-kms -c 2>/dev/null | awk '/connected/{print $1; exit}')
  CRTC=$(modetest -M hermes-kms -p 2>/dev/null | awk '/^[0-9]+/{print $1; exit}')
  echo "### connector=$CONN crtc=$CRTC — driving 60Hz modeset for ~5s"
  echo "### (vblank timer should fire ~300 times; -v shows vsync fps)"
  # -s sets the mode on the connector; -v prints the measured vblank/flip rate.
  timeout 5 modetest -M hermes-kms -s "${CONN}@${CRTC}:1920x1080-60" -v 2>&1 | grep -iE "freq|fps|setting|error|hz" | head -8
  echo "### vblank counters from the driver after the run:"
  dmesg | grep -iE "enabled virtual display|vblank|first active scanout" | tail -5
fi

echo "### dmesg — anything bad?"
dmesg | grep -iE "BUG|lockup|WARNING|RIP|invalid context|sleeping|circular locking|hermes" | tail -30

echo "### unloading (must succeed cleanly — no refcount leak)"
rmmod hermes_kms && echo "rmmod OK" || echo "rmmod FAILED (refcount leak?)"

echo "### final dmesg tail"
dmesg | tail -5
