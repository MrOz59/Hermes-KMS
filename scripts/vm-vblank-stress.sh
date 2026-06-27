#!/usr/bin/env bash
# Stress test INSIDE the virtme-ng VM: repeated load / modeset / unload cycles
# plus a longer modeset to exercise the vblank timer heavily, under lockdep +
# DEBUG_ATOMIC_SLEEP. Lifecycle bugs (refcount leaks, lock issues, timer races)
# show up across repeated cycles.
set -u
REPO=/home/ozzy/Projects/Hermes-KMS
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
FAIL=0

echo "### kernel: $(uname -r)"

for i in 1 2 3 4 5; do
  echo "### cycle $i: insmod"
  if ! insmod "$KO" initial_enabled=1 hotplug_events=0 \
       initial_width=1920 initial_height=1080 initial_refresh_hz=60; then
    echo "### cycle $i: INSMOD FAILED"; FAIL=1; break
  fi
  sleep 0.3
  CONN=$(modetest -M hermes-kms -c 2>/dev/null | awk '/connected/{print $1; exit}')
  CRTC=$(modetest -M hermes-kms -p 2>/dev/null | awk '/^[0-9]+/{print $1; exit}')
  # Hold a modeset for 2s so the vblank timer fires ~120 times, then drop it.
  timeout 2 modetest -M hermes-kms -s "${CONN}@${CRTC}:1920x1080-60" -v >/dev/null 2>&1
  if ! rmmod hermes_kms; then
    echo "### cycle $i: RMMOD FAILED (leak?)"; FAIL=1; break
  fi
  echo "### cycle $i: OK"
done

echo "### refresh-rate sweep (60/120/144) in one load:"
insmod "$KO" initial_enabled=1 hotplug_events=0 2>/dev/null
CONN=$(modetest -M hermes-kms -c 2>/dev/null | awk '/connected/{print $1; exit}')
CRTC=$(modetest -M hermes-kms -p 2>/dev/null | awk '/^[0-9]+/{print $1; exit}')
for hz in 60 120 144; do
  echo "### modeset 1920x1080@${hz}"
  timeout 2 modetest -M hermes-kms -s "${CONN}@${CRTC}:1920x1080-${hz}" -v 2>&1 | grep -iE "setting|error|freq" | head -2
done
rmmod hermes_kms && echo "### final rmmod OK" || { echo "### final rmmod FAILED"; FAIL=1; }

echo "### ===== dmesg: any splat? ====="
dmesg | grep -iE "BUG:|WARNING:|RIP:|invalid context|sleeping|circular locking|deadlock|irqs disabled|refcount|use-after-free|leak" | head -20 || true

echo "### RESULT: $([ $FAIL -eq 0 ] && echo PASS || echo FAIL)"
