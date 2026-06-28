#!/usr/bin/env bash
# Post-reboot: build the NEW Hermes-KMS driver (cursor plane + damage), load it,
# prepare Apollo, and start it ready to stream. Run this once after a clean boot
# so no stale module is wedged.
#
#   /home/ozzy/Projects/Hermes-KMS/scripts/host-fresh-test.sh
#
# Then connect from Hestia and start an app. The physical monitor must stay ON
# (Apollo fix) and the cursor must now show in the stream (new cursor plane).
#
# Recovery if the screen goes dark (run over SSH or a TTY, Ctrl+Alt+F3):
#   /home/ozzy/Projects/Hermes-KMS/scripts/host-restore-display.sh
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
APOLLO=/home/ozzy/Projects/Apollo-Linux/build/sunshine-0.0.0.c41437b6
LOG=/home/ozzy/hermes-stream-test.log
CONF="$HOME/.config/sunshine/sunshine.conf"

say() { printf '\n=== %s ===\n' "$*"; }

# 0. Refuse to run if a stale module is still wedged (should be clean post-boot).
if lsmod | grep -q '^hermes_kms'; then
  rc=$(awk '/^hermes_kms/{print $3}' /proc/modules)
  say "hermes_kms already loaded (refcount=$rc)"
  echo "Reboot first so the new module loads clean, or: sudo rmmod hermes_kms"
  echo "(if rmmod fails with refcount held by KWin, a relogin/reboot is needed)"
  exit 1
fi

# 1. Build the new driver. The Makefile auto-detects clang vs gcc from the
#    running kernel's config, so plain `make modules` is correct here.
say "building the new driver (cursor plane + damage)"
make -C "$REPO" modules || { echo "BUILD FAILED"; exit 1; }

# 2. Sanity: confirm this .ko actually contains the new cursor-plane code, so we
#    know we're loading today's build and not a stale artifact.
if strings "$KO" | grep -q "cursor"; then
  echo "ok: built module references the cursor plane"
else
  echo "WARNING: built module has no 'cursor' string -- is this the new source?"
fi

# 3. Config sanity for the no-blank test: hermes_kms backend, isolated OFF.
say "config check"
grep -E "virtual_display_backend|isolated_virtual_display" "$CONF" || {
  echo "config missing keys; expected virtual_display_backend = hermes_kms"; }
if grep -qE "isolated_virtual_display_option\s*=\s*enabled" "$CONF"; then
  echo "NOTE: isolated mode is ENABLED -- the physical monitor WILL go dark once"
  echo "      the session starts (by design). Disable it to keep the monitor on."
fi
if ! grep -qE "virtual_display_backend\s*=\s*hermes_kms" "$CONF"; then
  echo "ABORT: virtual_display_backend is not hermes_kms; fix the config first." ; exit 1
fi

# 4. Load the new module (compositor-driven: initial_enabled so the connector
#    is live; no hotplug suppression, no seat-ignore rule -> KWin adopts it).
say "loading hermes_kms"
sudo insmod "$KO" initial_enabled=1 || { echo "insmod FAILED"; exit 1; }
echo "loaded: $(lsmod | grep hermes_kms)"

# 5. Confirm the cursor plane + two planes are present on the live device.
say "verifying planes on the live device (expect Primary + Cursor)"
if command -v modetest >/dev/null; then
  modetest -M hermes-kms -p 2>/dev/null \
    | awk '/^Planes:/{p=1} p && /type:/{getline; getline; print "  plane type value:", $2}' \
    | head -4 || true
  modetest -M hermes-kms -p 2>/dev/null | grep -q "value: 2" \
    && echo "  ok: cursor plane (type=2) present" \
    || echo "  WARNING: no cursor plane found in modetest output"
fi

# 6. Prepare and launch Apollo.
say "preparing Apollo"
pkill -9 apollo sunshine 2>/dev/null; sleep 1
sudo setcap cap_sys_admin+p "$APOLLO" 2>/dev/null
sudo cp -a /home/ozzy/Projects/Apollo-Linux/build/assets/web/. /usr/share/apollo/web/ 2>/dev/null

say "starting Apollo"
: > "$LOG"
"$APOLLO" >>"$LOG" 2>&1 &
echo "Apollo PID $!. Web UI: https://localhost:47990"
echo
echo "NOW: connect from Hestia and START AN APP."
echo "  - physical monitor should STAY ON (Apollo fix)"
echo "  - cursor should now appear in the stream (new cursor plane)"
echo "Tail the log:     tail -f $LOG"
echo "Recover anytime:  $REPO/scripts/host-restore-display.sh"
