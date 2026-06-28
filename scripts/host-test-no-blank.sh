#!/usr/bin/env bash
# Validate the Apollo fix: creating the virtual display must NOT blank the
# physical monitor when isolated mode is OFF. Run this, then connect from
# Hestia and start an app; watch the physical screen + the log.
#
# Recovery if the screen still goes dark (run over SSH or a TTY):
#   scripts/host-restore-display.sh
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APOLLO=/home/ozzy/Projects/Apollo-Linux/build/sunshine-0.0.0.c41437b6
LOG=/home/ozzy/hermes-stream-test.log
CONF="$HOME/.config/sunshine/sunshine.conf"

echo "===== $(date) Apollo no-blank test ====="
echo "### config:"; grep -E "virtual_display_backend|isolated_virtual_display" "$CONF"

# Hard requirement for this test: isolated mode OFF (so the only thing that
# could move the desktop is the create-time layout we just fixed).
if grep -qE "isolated_virtual_display_option\s*=\s*enabled" "$CONF"; then
  echo "### ABORT: isolated_virtual_display_option is enabled; this test needs it disabled."
  exit 1
fi

echo "### physical layout BEFORE:"
kscreen-doctor -o 2>/dev/null | grep -E "Output:|enabled|priority"

pkill -9 apollo sunshine 2>/dev/null; sleep 1
sudo setcap cap_sys_admin+p "$APOLLO" 2>/dev/null

exec > >(tee -a "$LOG") 2>&1
echo "### starting Apollo build $APOLLO"
"$APOLLO" >>"$LOG" 2>&1 &
echo "### Apollo PID $!. Web UI: https://localhost:47990"
echo
echo "### NOW: connect from Hestia and START AN APP (that triggers createVirtualDisplay)."
echo "### WATCH the physical monitor -- with the fix it must STAY ON."
echo "### Tail the log live:  tail -f $LOG"
echo "### Expected log line:  [VDISPLAY/KScreen] Enabled Hermes-KMS output <name> (physical output left untouched until the session starts)"
echo "### Recover anytime:    $REPO/scripts/host-restore-display.sh"
