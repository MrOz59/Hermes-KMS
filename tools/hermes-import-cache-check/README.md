# Cross-process import-cache check

`hermes-import-cache-check` answers one driver question that a capture
consumer depends on when it keeps imports instead of re-importing every frame:

> Does an EGLImage of a DMA-BUF that **another process** allocated keep showing
> what that process renders into the buffer later, or only what the buffer held
> when the image was first sampled?

Hermes's NVENC DMA-BUF consumer (`experimental/nvidia-zero-copy`) keeps one
import per scanout buffer, keyed by dma_buf identity. That removes per-frame
`eglCreateImage`/`eglDestroyImage` work and bounds an NVIDIA problem that
matches capture exactly: on 595.71 through at least 610.43, sampling a
cross-process DMA-BUF import leaves importer VRAM behind after the image is
destroyed, reclaimed only at process exit. It is only correct if sampling a
cached image reads the live buffer. A reported, unconfirmed explanation of the
VRAM retention is a private copy made on first sample, which is exactly what
would make cached images stale, so this has to be measured per driver.

## How it works

No Hermes-KMS card or compositor is involved:

1. The tool forks before touching EGL. The **child** opens `--gpu`, allocates
   `--buffers` modified GBM BOs (any renderable layout, or linear with
   `--modifiers linear`) and sends their DMA-BUFs to the parent over a
   socket (`SCM_RIGHTS`).
2. For every round and buffer, the parent asks the child to render the
   `hermes-gpu-scanout-test` shader pattern with a new seed into that buffer.
   The child finishes the work (`glFinish`) before acknowledging.
3. The **parent** imports each buffer once, keeps the import, and after every
   render blits the cached image into its own RGBA8 target and checks every
   pixel against the new seed (`pattern.h`, including the suffix beyond 2 MiB).
   Unless `--no-fresh` is given, it also imports the same buffer anew and
   checks that, which separates image-level staleness from buffer-level
   problems.

Results are classified per re-sample as **current**, **stale** (matches the
previous render or the render at import time) or **corrupt** (matches
neither). First imports are checked too, so a layout that imports wrongly
regardless of caching (the system-memory corruption `hermes-egl-import-check`
documents) is reported as such rather than blamed on the cache.

`--churn N` afterwards imports, samples and destroys one buffer N times, the
per-frame pattern Hermes used before caching. It prints the cost per cycle;
watch `nvidia-smi --query-gpu=memory.used --format=csv -l 1` meanwhile to see
whether the driver retains memory per import.

## Build and run

```sh
make tools/hermes-import-cache-check/hermes-import-cache-check
tools/hermes-import-cache-check/hermes-import-cache-check --gpu /dev/dri/renderDN
tools/hermes-import-cache-check/hermes-import-cache-check --gpu /dev/dri/renderDN --modifiers linear
tools/hermes-import-cache-check/hermes-import-cache-check --gpu /dev/dri/renderDN --churn 2000
```

Needs libdrm, GBM with `gbm_bo_create_with_modifiers2`, EGL with GBM,
surfaceless/no-config contexts and DMA-BUF modifier import, and desktop GL
3.3. Non-NVIDIA GPUs are rejected unless `--allow-other-gpu` is given (useful
as a control on AMD/Intel); software rasterizers are always rejected. The
default 2560x1440 frame is larger than the 2 MiB boundary of the known NVIDIA
system-memory corruption; `--width`/`--height` change it.

Every report starts with the loaded NVIDIA kernel module's version and flavour.
The retention this check watches for was reported on 595.71 through 610.43,
all older than the 2026-09-30 security releases (580.178.04, 595.91.07,
610.57.04, 615.71.09), so a result is only comparable with one from the same
driver.

| Exit | Meaning |
| --- | --- |
| 0 | every cached re-sample showed the latest completed render |
| 1 | stale or corrupt samples, wrong first/fresh imports, or a setup failure |
| 2 | invalid arguments, or no cached re-sample ran |

On a failure the tool prints what follows for Hermes: cached-only problems
mean `HERMES_CUDA_EGL_IMPORT_CACHE=0`; problems with fresh imports too mean
per-frame imports would not help and the CPU copy stays.

## Scope

Writes complete before sampling, so this measures import coherence, not
implicit/explicit fence handling or a live compositor's scheduling. It does
not encode with NVENC and does not establish physical memory placement. It
complements `hermes-gpu-scanout-test` (storage passthrough through a
Hermes-KMS card) and `hermes-egl-import-check` (the GL/CUDA consumer chain on
a live frame). Branch CI compiles the tool and checks argument handling only;
hardware results are pending.
