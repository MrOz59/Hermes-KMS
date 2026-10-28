# GPU-origin passthrough experiment

`hermes-gpu-scanout-test` allocates an explicitly modified GBM BO on the selected
GPU and renders a deterministic, page-varying SDR pattern into it with GL. No
CPU pixel upload, source `mmap`, dumb buffer or udmabuf is used. It then:

1. PRIME-imports the BO into an explicitly selected Hermes-KMS primary node.
2. Commits the framebuffer to a dedicated virtual output at 60 Hz.
3. Acquires it through a token-bound Hermes-KMS render descriptor and waits for
   the returned sync_file.
4. Requires the original DMA-BUF `st_dev`/`st_ino`, dimensions, format, modifier,
   plane count, pitches and offsets to survive the round trip unchanged. On a
   UAPI 15 driver it also requires both `ACQUIRE_FRAME` and `GET_STATUS` to
   report the scanout as imported and to name the same exporter
   (`PASS: scanout reported as imported from nvidia_drm`); older drivers print
   `SKIP`.
5. Imports the returned descriptors on the originating GPU, blits to an own
   RGBA8 target, and checks every pixel against the shader's expected pattern.
   The source is checked independently before scanout, separating producer
   problems from capture/import problems. Readback is diagnostic only.
6. Disables scanout/output and destroys the actual GL/EGL/GBM resources. There
   is no skipped-teardown success path.

The default 2560x1440 image is larger than the observed 2 MiB corruption
boundary. Patterns vary in both coordinates; a duplicated or wrong suffix is
not hidden by a solid fill. The producer remains static after rendering and
uses `glFinish` before KMS: this isolates storage passthrough and does **not**
validate concurrent compositor/consumer scheduling or benchmark latency.

## Build and run

Needs libdrm, GBM (with `gbm_bo_create_with_modifiers2`), desktop GL 3.3 and EGL
with GBM, surfaceless/no-config contexts and DMA-BUF modifier import. NVIDIA's
GBM backend and DRM KMS must be available. The tool reports the loaded NVIDIA
kernel module's version and flavour (`nvidia driver: 610.57.04 (open kernel
module)`), DRM driver, GL vendor/renderer and allocated layout. It rejects non-NVIDIA drivers by default;
`--allow-other-gpu` permits a hardware control test, rejecting known Mesa
software rasterizers.

```sh
make tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test
make check-gpu-pattern
tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test \
  --device /dev/dri/cardN --gpu /dev/dri/renderDN
```

Replace both nodes explicitly. Use a dedicated **unused, disabled,
single-output** Hermes-KMS card, with permission to become its DRM master.
An existing master/session/active output is rejected before changing output
state. No real GPU primary node is modeset. Width/height can be overridden
with `--width N --height N`, within the driver's mode limits.

All allocated memory planes (up to four) are passed explicitly. Some modifier
layouts may still be rejected by the kernel framebuffer implementation or
reported with a different plane count; these fail the experiment rather than
being flattened, relabelled linear, or copied into system memory. The producer
does not change the driver's advertised modifiers or any kernel ABI.

## Which layout, and what a compositor could choose

By default the BO may use any modifier the GPU can render into
(`--modifiers egl`). A compositor is more constrained: when KWin renders on
one GPU and imports the result into another device's output, it intersects
that output plane's `IN_FORMATS` with the render GPU's non-external-only
modifiers. Hermes-KMS accepts any layout at `ADDFB2`, but advertises only
linear unless `scanout_modifiers=` lists more, so with the defaults KWin can
only keep a GPU-origin scanout if the GPU renders linear buffers. Two modes
reproduce that:

- `--modifiers plane` allocates only from the GPU's renderable modifiers that
  the card's primary plane advertises for ARGB8888. An empty intersection
  fails with the reason: KWin would fall back to a CPU copy into a dumb
  buffer.
- `--modifiers linear` allocates linear only, the default-configuration case.

`--list-modifiers` prints, without touching any output or needing DRM master:
the GPU's renderable and external-only ARGB8888/XRGB8888 modifiers, decoded
(NVIDIA block-linear fields, including the GB20x sector layouts); the card's
plane `IN_FORMATS` and the KWin-style intersection when `--device` is given;
and a ready `scanout_modifiers=` value (non-linear renderable layouts, at most
15). Advertising NVIDIA layouts lets the compositor scan out tiled buffers
that only a GPU import can read, so Hermes's NVENC sessions then need
`HERMES_KMS_EXPERIMENTAL_NVENC_DMABUF=auto` (or `1`), and encoders on other
GPUs cannot use those frames.

```sh
tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test \
  --list-modifiers --gpu /dev/dri/renderDN --device /dev/dri/cardN
tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test \
  --device /dev/dri/cardN --gpu /dev/dri/renderDN --modifiers plane
```

The modifier decoding, the intersection and the `IN_FORMATS` blob parser are
covered without a GPU by `make check-gpu-modifiers` (`tests/gpu-modifiers.c`).

## Attach the GL/CUDA checker

Keep the static producer alive in terminal A, publishing a private capture
credential in an existing directory owned by your user:

```sh
tools/hermes-gpu-scanout-test/hermes-gpu-scanout-test \
  --device /dev/dri/cardN --gpu /dev/dri/renderDN \
  --hold-seconds 120 --session-file "$XDG_RUNTIME_DIR/hermes-gpu-test.session"
```

Wait for `READY`. In terminal B, on the same GPU, run the CUDA-enabled checker:

```sh
tools/hermes-egl-import-check/hermes-egl-import-check \
  --device /dev/dri/cardN --gpu /dev/dri/renderDN \
  --session-file "$XDG_RUNTIME_DIR/hermes-gpu-test.session" \
  --expect-gpu-pattern --full-teardown
```

Finish consumers before the hold expires or stopping the producer. The
credential is created mode 0600 without replacing an existing file, never
printed, and removed during cleanup. The owner session is disabled on exit;
SIGINT/SIGTERM request orderly cleanup. A forced kill can leave the credential
file behind; it cannot authorize a replacement session.

`--expect-gpu-pattern` avoids trying to map GPU-origin tiled storage and checks
every GL-readback pixel against the producer's fixed shader pattern instead.
Only use it with this producer, not an ordinary desktop. The checker then checks
the complete GL-to-CUDA image and real interop teardown. It can return success
only if the known pattern, CUDA comparison and requested cleanup pass. Its
current import path supports one memory plane; an auxiliary-plane layout is
rejected explicitly. Compile that tool with
`HAVE_CUDA=1` as described in its README. Use a disposable test session because
the previously observed NVIDIA teardown crash is specifically being tested.

Exit 0 means the producer's identity/layout/pixels and GL/EGL teardown passed;
1 is a test/setup/cleanup failure, 2 is invalid arguments. An interrupted run
returns 128 plus the signal. No output proves physical VRAM placement, absence
of hidden driver migration, NVENC encode/decode, KWin passthrough or complete
Hermes streaming. Run those subsequent stages on the real host before enabling
the experimental Hermes consumer for normal sessions.

## Design references

- [NVIDIA GBM backend requirements and tracing](https://download.nvidia.com/XFree86/Linux-x86_64/595.84/README/gbm.html):
  `__NV_GBM_TRACE_ENABLED=1` adds backend diagnostics.
- [Khronos DMA-BUF modifiers](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt):
  external-only layouts are excluded; offsets, pitches and modifier halves
  accompany every plane, including auxiliary planes.
- [Kernel buffer exchange contract](https://docs.kernel.org/userspace-api/dma-buf-alloc-exchange.html):
  format/modifier metadata is required; it does not establish memory placement.
- [NVIDIA GB20x modifiers](https://lkml.iu.edu/hypermail/linux/kernel/2510.3/06153.html):
  the block-linear sector layout gained bits 27:26 for Blackwell 8/16-bpp
  surfaces, so layouts are not interchangeable across pre-GB20x and GB20x.
- [KWin multi-GPU surfaces](https://lxr.kde.org/source/plasma/kwin/src/backends/drm/drm_egl_layer_surface.cpp):
  import modes Egl, Dmabuf, LinearDmabuf and DumbBuffer are tried in that
  order; the dma-buf modes filter the plane's modifiers by the render GPU's
  non-external-only ones.

Branch CI compiles the producer and runs CPU pattern/modifier/argument
regressions. It has no NVIDIA/KMS device, so hardware results remain
**unvalidated**.

Whether an import the consumer keeps across frames observes later renders by
another process is measured separately, without a Hermes-KMS card, by
[`hermes-import-cache-check`](../hermes-import-cache-check/README.md).
