# Vblank timer — VM test results (overnight session)

## Summary

The software vblank timer (branch `experimental-vblank-timer`) is now
**kernel-correct**: it passes a full stress run in a virtme-ng VM under a kernel
with `PROVE_LOCKING` + `DEBUG_ATOMIC_SLEEP`, with no lock/atomic/irq/refcount
splats. The host soft-locks were caused by a missing plane `.destroy`, now fixed.

The KWin-on-VM test hit a hard environment limit (no GPU + no software GL
driver installed), so full-FPS validation with a real compositor could not be
completed in the VM. It needs a host (or a GPU-passthrough VM) to measure.

## What passed (kernel correctness — DONE)

`scripts/vm-vblank-stress.sh` in the debug VM:
- 5x load / modeset / unload cycles — all clean
- modeset at 1920x1080 **@60, @120, @144** — configurable refresh works
- `rmmod` clean every cycle
- **zero** atomic-sleep / lock-ordering / irq-state / refcount splats
- RESULT: PASS

Root-cause fix that made this pass: `drm_plane_funcs.destroy = drm_plane_cleanup`
was missing. `drm_universal_plane_init()` WARNs on it, and the plane was never
cleaned up on unload → teardown left IRQs disabled → `rmmod` Killed. (commit
`a8272aa`)

## What blocked (KWin FPS in the VM — env limit, not a driver bug)

`scripts/vm-kwin-test.sh`:
- KWin **did** adopt Hermes-KMS via `KWIN_DRM_DEVICES` and produced the first
  scanout frame ("first active scanout framebuffer ... enabled virtual display
  1920x1080@60") — the compositor-driven path works.
- But KWin then stalled at 1 frame (measured FPS ~0). Cause:
  `MESA: error: ZINK: vkEnumeratePhysicalDevices failed` / `failed to choose
  pdev`. The VM has no GPU; KWin 6 needs EGL/GL; Mesa has no `llvmpipe`
  (software GL) installed on this host (`/usr/lib/dri/*llvmpipe*` absent — only
  `radeonsi`), and Zink (GL-on-Vulkan) can't find a Vulkan device.
- So KWin can't render continuously in the VM → can't measure 60fps there.

This is an environment limitation. Options to actually measure compositor FPS
with the vblank timer:
1. **Test on the host** now that the kernel-level bugs are fixed and validated
   under lockdep (lower risk than before). Marking the connector `non_desktop`
   and/or loading isolated should stop KWin from stealing the physical desktop.
2. Install a software GL stack in the VM: `mesa` with `llvmpipe`
   (`vulkan-swrast`/`lavapipe` so Zink has a Vulkan device, or force GL via a
   working `swrast`/`llvmpipe` DRI module).
3. GPU passthrough to the VM (vfio) — heavyweight.

## DECISIVE RESULT: vblank fires at exactly 60Hz

`modetest -M hermes-kms -s <conn>@<crtc>:1920x1080-60 -v` in the VM measures the
real vblank rate from the delivered events:

```text
freq: 60.55Hz
freq: 60.00Hz
freq: 60.00Hz
freq: 60.00Hz
freq: 60.00Hz
```

All three refresh rates verified (modetest -v, mode matched by name so the CVT
preferred mode is used):

```text
target  60Hz -> freq: 60.00Hz
target 120Hz -> freq: 119.96 / 119.97Hz
target 144Hz -> freq: 143.90 / 143.91Hz
```

The software vblank timer delivers a rock-steady tick at the requested refresh
(60/120/144Hz — configurable). This is the whole point: the compositor now gets
a real per-frame "present now" signal at the mode's refresh, which is what was
missing (no_vblank=true → compositor paced by its commit/ack loop at ~40fps).

Note: a Weston run with `--renderer=pixman` adopted Virtual-1 and scanned out,
but `frame_sequence` stayed at 1 because a shadow-framebuffer compositor with no
real damage doesn't page-flip continuously — that's a test artifact, not a vblank
problem. The vblank rate itself (measured directly by modetest -v) is correct.

## Recommended next step (host test — for the morning)

The driver is **kernel-validated and lockdep-clean**; the vblank fires at
60/120/144Hz. The only remaining question — does the vblank make KWin compose a
game at full refresh instead of ~40 — needs a real GPU, so it must be tested on
the host. The kernel-level bugs that soft-locked the host are fixed, so this is
much lower risk now.

To test on the host WITHOUT blanking the physical monitor, the open question is
the KWin-steals-the-desktop problem. Two things to try, in order:

1. Load with `non_desktop=1`:
   `sudo insmod hermes_kms.ko initial_enabled=1 non_desktop=1`
   A non-desktop connector is not adopted as a session monitor by KWin. RISK:
   it may also stop KWin composing on it at all (EVDI is NOT non_desktop and
   relies on Apollo's kscreen-doctor to place the virtual output as secondary).
   So test whether the stream still works with non_desktop=1.
2. If non_desktop breaks streaming, keep non_desktop=0 but let Apollo manage the
   layout via kscreen-doctor (as it already does for EVDI) so the virtual output
   is added as secondary, not made primary. The blank-screen happened in the
   bare modetest/isolated test where nothing manages the layout — it may not
   happen in the real Apollo flow.

DO NOT change the non_desktop default blind — it needs a host test to confirm it
doesn't break the streaming capture path. Decide with a real test.

Measure compositor FPS on the host while a game streams: the driver's
`frame_update_count` (via `hermes-kmsctl metrics`) advances once per page-flip,
so its rate is the real compositor FPS. Compare against the pre-vblank ~40 and
against EVDI (~40 with ~2x our spikes).
