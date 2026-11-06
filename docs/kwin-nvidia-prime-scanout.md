# KWin 6.8 and NVIDIA: why the virtual output is still CPU copied

Status: **research / proposed compositor work**. This document does not
enable zero-copy in KWin and is not a claim that any change to KWin has
been built, tested, or merged.

## Confirmed behavior on RTX 5060 Ti

The 2026-11-06 RTX 5060 Ti (GB206) / NVIDIA Open 615.78.08 /
KWin 6.8.2 tester used Hermes-KMS experimental UAPI 15, including a
module configuration advertising the 12 NVIDIA block-linear scanout
modifiers. The real compositor still supplied
`modifier 0x0 (Hermes-KMS dumb buffer)` to Hermes, which correctly chose
CPU copy with `HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto`.

Without KWin, the same GPU successfully created a block-linear
`0x0300000000e08014` buffer, PRIME-imported it into Hermes-KMS,
preserved the exporter identity, and verified pixels including past 2 MiB
through EGL/GL/CUDA. The import cache was correct (117/117) and teardown
did not retain VRAM. These isolated tests **do not validate a live
zero-copy stream**, but they demonstrate that a direct scanout import
path is viable on the tested driver and hardware.

The new four-entry-per-plane Hermes-KMS export LRU was then tested
during CPU-copy streaming: **30,927 export hits, 6 misses**, zero
DMA-BUF mmap faults in the synchronous capture thread, and 10.5% CPU
usage vs 33.8% before. The kernel cache is no longer the principal
blocking issue.

## KWin source analysis: Plasma/6.8

The relevant upstream files are:

- [`src/backends/drm/drm_egl_layer_surface.cpp`](https://github.com/KDE/kwin/blob/Plasma/6.8/src/backends/drm/drm_egl_layer_surface.cpp)
  (around `createSurface` lines 386-495)
- [`src/multigpuswapchain.cpp`](https://github.com/KDE/kwin/blob/Plasma/6.8/src/multigpuswapchain.cpp)
  (`MultiGpuSwapchain::createForScanout`, around line 318)
- [`src/backends/drm/drm_gpu.cpp`](https://github.com/KDE/kwin/blob/Plasma/6.8/src/backends/drm/drm_gpu.cpp)
  (`DrmGpu::importBuffer`, around line 955; `updateRenderDevice`, line 1076)
- [`src/core/gpumanager.cpp`](https://github.com/KDE/kwin/blob/Plasma/6.8/src/core/gpumanager.cpp)
  (`compatibleRenderDevice`).

In KWin 6.8, when the KMS target's render device differs from the
compositor's render device, `EglGbmLayerSurface::createSurface`
attempts these modes:

1. Special-case `MultiGpuImportMode::None` only if the different
   KMS devices are in fact backed by the **same** EGL render node.
2. `MultiGpuImportMode::GpuCopy`, which explicitly rejects any
   missing or software `m_gpu->renderDevice()`, and requires
   `MultiGpuSwapchain::createForScanout` to find a
   `GpuManager::compatibleRenderDevice(targetDevice)`.
3. `MultiGpuImportMode::DumbBuffer`, which uses a CPU-managed
   `QPainterSwapchain` as fallback.

The Hermes-KMS render node exists for KMS control/capture. It does not
offer a real EGL/Vulkan rendering engine; KWin is therefore unlikely to
register it as a compatible non-software `RenderDevice`. That predicts
the observed dumb-buffer fallback **even when all NVIDIA modifiers
intersect the plane's advertised formats**. This exact failed
conditional has not yet been captured in the tester's KWin log.

The first special case cannot solve this by itself: the actual NVIDIA
render node and the Hermes-KMS capture node are not the same render
device. Also, `MultiGpuImportMode::None` in the current allocation
path creates a source swapchain through the target DRM allocator;
simply forcing that enum is not a valid solution.

**Important distinction:** `DrmGpu::importBuffer` already has a direct
PRIME path (`drmPrimeFDToHandle` and
`drmModeAddFB2WithModifiers`) that can register a producer DMA-BUF as
a KMS framebuffer. The tests proved this succeeds for an NVIDIA
block-linear buffer on Hermes-KMS in isolation. The missing compositor
feature is *selecting and allocating that path without a destination
render engine*, not implementing PRIME in the kernel driver.

## Proposed KWin-side solution (not implemented here)

An upstreamable KWin change should add an explicit direct-dmabuf
scanout candidate for KMS targets that support PRIME imports but do
not expose GPU rendering:

1. Allocate/render on the **source NVIDIA GPU**, using a format and
   modifier in the intersection of source renderable modifiers and
   target primary-plane `IN_FORMATS`. Do not treat an NVIDIA
   `DRM_FORMAT_MOD_LINEAR` marked external-only as renderable.
2. Use the target's existing `DrmGpu::importBuffer` to register the
   rendered source DMA-BUF with the virtual KMS card. This must not
   rely on a destination EGL/Vulkan `RenderDevice`.
3. Verify framebuffer creation and **atomic scanout** before selecting
   the new mode. On unsupported format/modifier, PRIME import or
   commit failure, fall back to the existing dumb-buffer path. Avoid
   per-frame mode-switch loops and repeated failed PRIME imports.
4. Preserve buffer lifetime and synchronization correctly. The source
   must not recycle buffers before scanout is complete; propagate
   ready fences, damage, pixel format, HDR metadata and cursor
   handling as required. Exercise disconnect, mode switches and
   teardown under sustained streaming.
5. Do not apply a blanket bypass to all multi-GPU devices. Driver
   combinations have known reset/compatibility hazards. Gate the
   experiment behind an explicit switch until tested; any eventual
   upstream interface should be based on device capabilities and
   checked buffer import, not a hard-coded GPU vendor assumption.

The existing `GpuCopy` code is still appropriate for ordinary
cross-GPU displays where a real destination GPU must receive its own
scanout buffer. The virtual Hermes-KMS target is different because
it only exposes the compositor's framebuffer to a separate consumer.

### Why KWin environment toggles are not sufficient

`KWIN_ALLOW_VULKAN_MGPU_COPIES` changes which copy API KWin may
prefer, but **does not create** a missing destination render device.
`KWIN_DRM_DEVICES` can change GPU selection/order, not add GPU
rendering to Hermes-KMS. Enabling additional `scanout_modifiers`
on Hermes-KMS fixes a necessary format intersection but cannot
by itself bypass the KWin `GpuCopy` destination check.

Do not recommend experimental combinations of these options as a
validated zero-copy fix.

## Diagnostics to request from a tester

KWin's DRM log category is
`kwin_wayland_drm` (see `src/backends/drm/drm_logging.cpp`).
For a *controlled* reproduction, enable
`QT_LOGGING_RULES="kwin_wayland_drm.debug=true"` **in the KWin
process environment before it starts**. Adding it only to
`hermes.service` does nothing for the compositor. Changing the KWin
session environment can disrupt the desktop; testers should plan for a
session restart and preserve the existing settings.

Useful KWin messages include `Couldn't find any render device for`,
`chose cpu import`, and PRIME import/framebuffer errors. Collect
them with `journalctl --user -b` after the relevant test. Include
the active KWin version and GPU/render-node order.

From the Hermes/driver side, collect:

- Hermes's `NVENC auto route` modifier and exporter origin.
- `hermes_kms_stats` framebuffer origin, modifier, frame-export hits,
  misses **and evictions**.
- `HERMES_KMS_CPU_COPY_DIAGNOSTICS=1` output for the CPU fallback.
- `HERMES_CUDA_UPLOAD_DIAGNOSTICS=1` output when testing async upload,
  while keeping `HERMES_CUDA_ASYNC_UPLOAD=0` or unset for normal use.

## Known exclusions

`hermes-sysmem-import-check --verify` failed on NVIDIA 615.78.08:
system-memory DMA-BUF pixels were incorrect from byte 64 even with
THP; `eglDestroyImage` then segfaulted inside the NVIDIA EGL driver.
**Do not force system-memory DMA-BUF import** to work around KWin's
dumb-buffer fallback. The viable target is direct scanout of an
NVIDIA-origin buffer, not treating a dumb buffer as GPU-origin.

Real-session NVIDIA zero-copy is still experimental and unvalidated.
