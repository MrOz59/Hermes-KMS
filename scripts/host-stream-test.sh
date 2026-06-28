#!/usr/bin/env bash
# Host streaming test for Hermes-KMS (NOT EVDI). Logs everything persistently so
# nothing is lost on a crash/reboot. Designed to be driven over SSH so the
# physical screen going dark does not block recovery.
#
# Recovery if the screen goes dark (run over SSH or from a TTY):
#   scripts/host-restore-display.sh
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"
APOLLO=/home/ozzy/Projects/Apollo-Linux/build/sunshine-0.0.0.c41437b6
LOG=/home/ozzy/hermes-stream-test.log   # persistent (home survives reboot)

exec > >(tee -a "$LOG") 2>&1
echo "===== $(date) host stream test ====="

# 0. Sanity: ensure no other Apollo holds the port, and the right backend.
pkill -9 apollo sunshine 2>/dev/null; sleep 1
echo "### config backend: $(grep virtual_display_backend ~/.config/sunshine/sunshine.conf)"
echo "### isolated: $(grep isolated ~/.config/sunshine/sunshine.conf)"

# 1. Load Hermes-KMS (compositor-driven). Skip if already loaded.
if ! lsmod | grep -q '^hermes_kms'; then
  sudo insmod "$KO" initial_enabled=1 || { echo "insmod FAILED"; exit 1; }
fi
echo "### hermes loaded: $(lsmod | grep hermes_kms)"

# 2. setcap + assets for the build binary.
sudo setcap cap_sys_admin+p "$APOLLO"
sudo cp -a /home/ozzy/Projects/Apollo-Linux/build/assets/web/. /usr/share/apollo/web/

# 3. Run Apollo in the background, logging to the persistent file.
echo "### starting Apollo build (hermes_kms backend)"
"$APOLLO" >>"$LOG" 2>&1 &
echo "### Apollo PID $!. Web UI at https://localhost:47990"
echo "### Tail the log live with:  tail -f $LOG"
echo "### When done / if screen goes dark, recover with: $REPO/scripts/host-restore-display.sh"
