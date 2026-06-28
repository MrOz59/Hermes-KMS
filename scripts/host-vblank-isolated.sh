#!/usr/bin/env bash
# Safe isolated vblank test on the REAL host: validates the vblank timer fires
# at the mode's refresh via modetest, WITHOUT letting KWin/Xwayland touch the
# virtual display (hotplug_events=0 + seat-ignore rule). The physical monitor is
# never handed to the compositor, so even if something misbehaves the desktop
# stays up.
#
# Run: sudo ./scripts/host-vblank-isolated.sh
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo $0" >&2; exit 1
fi

# 0. Refuse to run if a stale module is wedged (refcount -1 needs a reboot).
if lsmod | grep -q '^hermes_kms'; then
  rc=$(awk '/^hermes_kms/{print $3}' /proc/modules)
  echo "hermes_kms already loaded (refcount=$rc). Unload it first:"
  echo "  sudo rmmod hermes_kms     # if it fails with -1, reboot is required"
  exit 1
fi

# 1. Install the seat-ignore udev rule so the compositor does not grab the card.
echo "### installing seat-ignore udev rule (isolated testing)"
install -m 0644 "$REPO/udev/99-hermes-kms-ignore-seat.rules" \
  /etc/udev/rules.d/99-hermes-kms-ignore-seat.rules
udevadm control --reload-rules

# 2. Load isolated: connected but silent, so modetest can drive it and the
#    compositor is not notified.
for hz in 60 120 144; do
  echo "### ===== target ${hz}Hz ====="
  insmod "$KO" initial_enabled=1 hotplug_events=0 \
         initial_width=1920 initial_height=1080 initial_refresh_hz="$hz" || {
    echo "insmod FAILED"; continue; }
  sleep 0.5

  # Confirm the compositor did NOT grab it (CURRENT_TAGS should have no seat).
  CARD=$(readlink -f "$(ls /dev/dri/by-path/*hermes*card 2>/dev/null | head -1)")
  echo "### hermes card: ${CARD:-not found}"
  TAGS=$(udevadm info "$CARD" 2>/dev/null | grep -E '^E: CURRENT_TAGS=' || true)
  case "$TAGS" in
    *:seat:*) echo "### WARNING: card is seat-tagged (compositor may grab it)";;
    *)        echo "### ok: card is not on the active seat";;
  esac

  CONN=$(modetest -M hermes-kms -c 2>/dev/null | awk '/connected/{print $1; exit}')
  CRTC=$(modetest -M hermes-kms -p 2>/dev/null | awk '/^[0-9]+/{print $1; exit}')
  echo "### connector=$CONN crtc=$CRTC — measuring vblank rate for ~4s"
  # Match the mode by name only so the preferred CVT mode (the requested hz) wins.
  timeout 4 modetest -M hermes-kms -s "${CONN}@${CRTC}:1920x1080" -v 2>&1 \
    | grep -oE "freq: [0-9.]+Hz" | sort | uniq -c | head -3

  echo "### dmesg check:"
  dmesg | grep -iE "BUG:|WARNING:|RIP:|sleeping|irqs disabled|circular" | tail -5 \
    || echo "### clean"

  rmmod hermes_kms && echo "### rmmod OK" || { echo "### rmmod FAILED"; break; }
  sleep 0.5
done

echo "### done. The seat-ignore rule is still installed; remove it for normal"
echo "### compositor-driven streaming: sudo make uninstall-dev-udev"
