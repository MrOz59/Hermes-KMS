# hermes-egl-import-check / pitch_detect

Diagnostic consumers for direct NVIDIA/EGL DMA-BUF import validation.
These tools describe `main`; successful kernel export or EGL image creation
alone is not proof of correct full-frame sampling, NVENC encoding or zero-copy.
Current Hermes uses a CPU-copy fallback for NVIDIA capture. See the
[roadmap](../../docs/roadmap.md) and [upstream references](../../docs/references.md).

## hermes_egl_import_check.c

Validates the NVENC-style consumer chain for a live Hermes-KMS frame:

```
ACQUIRE_FRAME (DMA-BUF) -> wait on the exported sync file
  -> eglCreateImage(EGL_LINUX_DMA_BUF_EXT)
  -> texture bind (OES, falling back to glEGLImageTargetTexStorageEXT)
  -> FBO readback (proves sampling)
  -> GL blit into an own texture -> cuGraphicsGLRegisterImage
     (mirrors the Sunshine/Hermes CUDA converter flow)
```

The tool follows the driver's synchronization contract: a successful
ACQUIRE_FRAME only means the frame was exported, so the checker waits on the
returned sync file (the framebuffer's write fence) before reading anything,
and brackets its CPU reference read with `DMA_BUF_IOCTL_SYNC`.

Every stage is reported individually and summarized at the end:

```
stage summary:
  frame acquisition:   PASS
  fence wait:          PASS | FAIL | NOT PROVIDED
  CPU reference read:  PASS | SKIPPED
  EGL import:          PASS | FAIL
  OpenGL validation:   PASS | FAIL
  CUDA registration:   PASS | FAIL | SKIPPED
```

Exit codes: `0` when the complete chain ran and passed, `1` when a stage
failed, `2` when nothing failed but skipped stages keep the chain
unvalidated (for example `--no-cuda` or a build without CUDA).

The CUDA device is selected with `cuGLGetDevices()` so it matches the GPU
behind the GL context (`--gpu`), not blindly device 0, and context handling
uses the primary-context API, which keeps the same signature across CUDA
11/12/13 (the unversioned `cuCtxCreate` does not).

Build (from the repository root, together with the other tools):

```bash
make tools                # CUDA stage compiled out
make tools HAVE_CUDA=1    # with the CUDA stage (needs cuda.h/cudaGL.h, libcuda)
```

Or standalone:

```bash
cc -O2 -Wall -DHAVE_CUDA -I ../../include/uapi -I/usr/include/libdrm \
  -o hermes-egl-import-check hermes_egl_import_check.c \
  -ldrm -lgbm -lEGL -lGL -lcuda
```

Options: `--session-file PATH` (required by the protected capture path),
`--device /dev/dri/renderDN` (Hermes capture node), `--output N` (1-based),
`--gpu /dev/dri/renderDN` (real import GPU), `--wait-ms MS`, `--no-cuda`, and
`--no-cpu-ref`. The capture node is opened `O_RDONLY`; the real GPU is a different
node with its normal rendering permissions. Identify nodes with `hermes-kmsctl`
and do not infer roles from a fixed render-node number.

In one terminal, keep an output owner alive:

```bash
session_file="${XDG_RUNTIME_DIR:?}/hermes-kms-import-session"
tools/hermes-kmsctl/hermes-kmsctl --session-file "$session_file" hold 2560x1440@60
```

Once a compositor is scanning out, run from another terminal:

```bash
session_file="${XDG_RUNTIME_DIR:?}/hermes-kms-import-session"
tools/hermes-egl-import-check/hermes-egl-import-check --session-file "$session_file"
tools/hermes-egl-import-check/pitch-detect --session-file "$session_file"
```

Use explicit `--device`/`--gpu` selections when several cards exist. These
checkers use legacy `ACQUIRE_FRAME` and do not validate UAPI v14 colour/HDR
interpretation; use the [HDR tests](../../docs/hdr-capture.md#reproducible-checks)
for that contract.

## pitch_detect.c

Compares the GPU's view of an imported frame against the CPU view of the
same DMA-BUF under several hypotheses (declared pitch, width*4, vertical
flip, R/B swap, brute-forced pitch scan, drift-filtered rows). Waits on the
exported sync file and brackets both CPU snapshots with
`DMA_BUF_IOCTL_SYNC` so the reference cannot be an in-flight frame.

Options: `--session-file PATH` (required), `--device /dev/dri/renderDN`,
`--gpu /dev/dri/renderDN`. The session file is the mode-0600 credential
published by the output owner (for example, via `hermes-kmsctl --session-file
PATH hold ...`). It authorizes this separate capture file descriptor without
coupling the tool or driver to any particular application.

## Known finding (2026-08, NVIDIA 595.84, RTX 5060 Ti)

On that tested configuration, the NVIDIA proprietary driver imported system-memory DMA-BUFs via
`EGL_EXT_image_dma_buf_import` (with the TexStorageEXT bind), but the
sampled content is only correct for roughly the first contiguous ~2 MB of
the buffer; beyond that the GPU reads the wrong pages while the CPU view
(`mmap`) of the very same DMA-BUF is pixel-perfect. Visible as diagonal
stripe corruption at larger modes. Current Hermes therefore retains a CPU-copy capture path for NVENC sessions.
This is a historical reproduction, not a claim that every NVIDIA GPU/driver
has the same defect; retest and record the exact kernel, driver, GPU, format,
modifier and full-frame result before changing the fallback.

### Narrowing it down

2 MiB is also the size of a huge page, and whether the import works has so
far depended on how contiguously the kernel happened to allocate the pages.
Two experiments separate the possible causes. Each takes a minute on an
NVIDIA machine; run them at a mode that shows the corruption, such as
2560x1440.

1. Without Hermes-KMS, with the udmabuf control in
   `tools/hermes-sysmem-import-check`. `--verify` reads the whole import back
   through the GPU, since the import itself has never failed:

   ```sh
   hermes-sysmem-import-check --verify 2560 1440            # scattered 4 KiB pages
   hermes-sysmem-import-check --verify --thp 2560 1440      # 2 MiB transparent huge pages
   sudo sh -c 'echo 16 > /proc/sys/vm/nr_hugepages'
   hermes-sysmem-import-check --verify --hugetlb 2560 1440  # reserved 2 MiB pages
   ```

   - All PASS: NVIDIA imports plain system memory correctly, and the fault is
     specific to what Hermes-KMS exports (how its scatter/gather table is
     built, for instance).
   - The first FAILs and the huge-page runs PASS: the importer only reads
     physically contiguous 2 MiB runs correctly. Experiment 2 then shows
     whether Hermes-KMS can provide them.
   - All FAIL: no producer-side arrangement fixes it; it belongs in a report to
     NVIDIA and the CPU copy stays.

   A FAIL prints where the correct prefix ends and which page the GPU read
   instead. On AMD (radeonsi) all three pass at 1440p and 4K.

2. With Hermes-KMS itself: load the driver with `huge_gem=1` and repeat
   `hermes-egl-import-check` and `pitch-detect` against a live output at the
   same mode. `huge_gem` backs locally allocated dumb buffers with 2 MiB folios where memory allows,
   on kernels that have `drm_gem_huge_mnt_create()`; dmesg says whether it
   took effect. In a VM a 1440p buffer got seven 2 MiB folios (all but its
   last partial block) and a 4K buffer fifteen, and the export-stress and HDR
   capture tests pass with it.

Related teardown finding: once CUDA/GL interop has touched the imported
buffer in a process, the same driver segfaults when the last GPU-side
reference to the import is released, regardless of teardown order
(`eglDestroyImage`, texture delete or `eglTerminate`, whichever comes
last). Without the CUDA stage the identical teardown runs clean. The
checker therefore skips the GPU object teardown after CUDA interop and
leaves the release to process cleanup via `_exit`, keeping its exit code
deterministic.

## Separate experimental branch

[`experimental/nvidia-zero-copy`](https://github.com/MrOz59/Hermes-KMS/tree/experimental/nvidia-zero-copy)
contains additional GPU-origin scanout, modifier, full-frame CUDA-content and
cross-process import-cache coherence probes. Their flags and availability must
be checked on that branch; they are not tools shipped by `main`. A synthetic
producer passing does not establish that KWin chooses the same allocation or
that a complete streaming/teardown path is safe to replace.
