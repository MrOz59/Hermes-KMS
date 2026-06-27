#!/usr/bin/env bash
# Attempt to run KWin Wayland on top of Hermes-KMS inside the virtme-ng VM, to
# reproduce the compositor adopting the virtual output and exercise the vblank
# timer through a real compositor (not just modetest). Software rendering
# (llvmpipe) because the VM has no GPU.
#
# This is exploratory: KWin needs EGL/GBM render, which on a GPU-less VM means
# llvmpipe, and it may or may not drive a render-only Hermes node. We capture
# whatever happens for analysis.
set -u
REPO=/home/ozzy/Projects/Hermes-KMS
KO="$REPO/kernel/hermes-kms/hermes_kms.ko"

echo "### kernel: $(uname -r)"
echo "### loading hermes_kms (compositor-driven: hotplug on, so KWin can adopt it)"
insmod "$KO" initial_enabled=1 || { echo "insmod FAILED"; exit 1; }
sleep 0.5
ls -l /dev/dri/ 2>/dev/null

# Force pure software GL (llvmpipe), and explicitly avoid Zink (GL-on-Vulkan),
# which fails on a GPU-less VM (no Vulkan device). KWin 6 may prefer Zink, so we
# pin the Mesa loader to the software rasterizer and hide Vulkan.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
export MESA_VK_DEVICE_SELECT=list
export KWIN_COMPOSE=Q   # force QPainter (software) compositing backend in KWin
export XDG_RUNTIME_DIR=/tmp/xdg-kwin
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
HERMES_CARD=$(ls /dev/dri/by-path/*hermes*card 2>/dev/null | head -1)
export KWIN_DRM_DEVICES="$(readlink -f "$HERMES_CARD" 2>/dev/null)"
echo "### KWIN_DRM_DEVICES=$KWIN_DRM_DEVICES"

echo "### starting kwin_wayland --drm for ~10s (capturing output)"
timeout 10 kwin_wayland --drm --no-lockscreen >/tmp/kwin-out.log 2>&1 &
KPID=$!

# Measure how many frames the compositor actually scans out over a window. The
# driver's frame_sequence advances once per compositor commit; sampling it tells
# us the real compositor FPS — the whole point of the vblank timer.
sleep 4
S1=$(dmesg | grep -oE "sequence=[0-9]+" | tail -1 | cut -d= -f2)
T1=$(date +%s.%N)
sleep 3
S2=$(dmesg | grep -oE "sequence=[0-9]+" | tail -1 | cut -d= -f2)
T2=$(date +%s.%N)
if [ -n "$S1" ] && [ -n "$S2" ]; then
  FPS=$(awk "BEGIN{d=$S2-$S1; t=$T2-$T1; if(t>0) printf \"%.1f\", d/t; else print \"?\"}")
  echo "### MEASURED COMPOSITOR FPS: ~$FPS (frames $S1 -> $S2 over ~3s)"
else
  echo "### could not sample frame_sequence (S1=$S1 S2=$S2)"
fi

# Let KWin exit and release the device before unloading.
kill "$KPID" 2>/dev/null
wait "$KPID" 2>/dev/null
sleep 1

echo "### dmesg: vblank/scanout/splat?"
dmesg | grep -iE "enabled virtual display|first active scanout|BUG:|WARNING:|sleeping|irqs disabled|circular" | tail -10
echo "### kwin output (errors only):"
grep -iE "error|fail|fatal|backend|drm|output|no usable" /tmp/kwin-out.log 2>/dev/null | head -15

for try in 1 2 3; do
  rmmod hermes_kms 2>/dev/null && { echo "### rmmod OK"; break; }
  echo "### rmmod busy, retry $try"; sleep 2
done
