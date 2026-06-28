#!/usr/bin/env bash
# RECOVERY: re-enable the physical monitor and stop the stream. Run over SSH or
# from a TTY (Ctrl+Alt+F3) if the physical screen goes dark.
set -u

echo "### stopping Apollo"
pkill -9 apollo sunshine 2>/dev/null
sleep 1

echo "### restoring physical monitor (HDMI-A-1) as primary, disabling Virtual-1"
kscreen-doctor output.HDMI-A-1.enable output.HDMI-A-1.priority.1 2>/dev/null
kscreen-doctor output.Virtual-1.disable 2>/dev/null

echo "### current layout:"
kscreen-doctor -o 2>/dev/null | grep -E "Output:|enabled|priority|connected" | head -12

echo "### done. If the screen is still dark, the compositor may need a kwin restart:"
echo "###   kwin_wayland --replace &    (from the session)"
