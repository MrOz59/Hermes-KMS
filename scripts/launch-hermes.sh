#!/usr/bin/env bash
# Install the freshly-built Hermes UI + binary and launch it, then open the UI.
# Needs sudo for setcap and copying into /usr/share. Run from your terminal:
#   /home/ozzy/Projects/Hermes-KMS/scripts/launch-hermes.sh
set -u

APP_DIR=/home/ozzy/Projects/Apollo-Linux
# Use the build symlink so it always points at the freshest binary
# (the hashed name changes every commit).
APOLLO="$APP_DIR/build/sunshine"
WEB_SRC="$APP_DIR/build/assets/web/"
WEB_DST=/usr/share/apollo/web/
LOG=/home/ozzy/hermes-stream-test.log

echo "### stopping any running instance"
pkill -9 apollo sunshine 2>/dev/null; sleep 1

echo "### setcap (KMS capture needs CAP_SYS_ADMIN)"
sudo setcap cap_sys_admin+p "$APOLLO" || { echo "setcap failed"; exit 1; }

echo "### syncing web assets (clean: --delete removes stale chunks from old builds)"
if command -v rsync >/dev/null; then
  sudo rsync -a --delete "$WEB_SRC" "$WEB_DST"
else
  sudo rm -rf "$WEB_DST" && sudo cp -a "${WEB_SRC%/}" "${WEB_DST%/}"
fi

echo "### starting Hermes"
: > "$LOG"
"$APOLLO" >>"$LOG" 2>&1 &
PID=$!
echo "### Hermes PID $PID"

# Wait until the web UI answers before opening the browser.
for i in $(seq 1 20); do
  code=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 2 https://localhost:47990/ 2>/dev/null)
  [ "$code" = "200" ] || [ "$code" = "302" ] || [ "$code" = "401" ] && break
  sleep 0.5
done
echo "### UI responding (http $code)"

echo "### opening the UI in your browser"
xdg-open "https://localhost:47990/" >/dev/null 2>&1 \
  || echo "open it manually: https://localhost:47990/"

echo
echo "### Done. If you see a stale page, hard-reload (Ctrl+Shift+R) or use an"
echo "### incognito window (Ctrl+Shift+N) to bypass the browser cache."
echo "### Tail logs: tail -f $LOG"
