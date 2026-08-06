# hermes-egl-import-check / pitch_detect

Diagnostic consumers for the NVENC/EGL side of the roadmap item
"NVENC/AMF DMA-BUF import validation".

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

Build:

```bash
cc -O2 -Wall -DHAVE_CUDA -I ../../include/uapi -I/usr/include/libdrm \
  -o hermes-egl-import-check hermes_egl_import_check.c \
  -ldrm -lgbm -lEGL -lGL -lcuda
```

Options: `--device /dev/dri/cardN` (Hermes node), `--gpu /dev/dri/renderDN`
(import GPU), `--wait-ms MS`, `--no-cuda`.

## pitch_detect.c

Compares the GPU's view of an imported frame against the CPU view of the
same DMA-BUF under several hypotheses (declared pitch, width*4, vertical
flip, R/B swap, brute-forced pitch scan, drift-filtered rows). Waits on the
exported sync file and brackets both CPU snapshots with
`DMA_BUF_IOCTL_SYNC` so the reference cannot be an in-flight frame.

Options: `--device /dev/dri/cardN`, `--gpu /dev/dri/renderDN`.

## Known finding (2026-08, NVIDIA 595.84, RTX 5060 Ti)

The NVIDIA proprietary driver imports these system-memory DMA-BUFs via
`EGL_EXT_image_dma_buf_import` (with the TexStorageEXT bind), but the
sampled content is only correct for roughly the first contiguous ~2 MB of
the buffer; beyond that the GPU reads the wrong pages while the CPU view
(`mmap`) of the very same DMA-BUF is pixel-perfect. Visible as diagonal
stripe corruption at larger modes. The Hermes NVIDIA fork therefore uses a
CPU-copy capture path for NVENC sessions until this is resolved.

Related teardown finding: once CUDA/GL interop has touched the imported
buffer in a process, the same driver segfaults when the last GPU-side
reference to the import is released, regardless of teardown order
(`eglDestroyImage`, texture delete or `eglTerminate`, whichever comes
last). Without the CUDA stage the identical teardown runs clean. The
checker therefore skips the GPU object teardown after CUDA interop and
leaves the release to process cleanup via `_exit`, keeping its exit code
deterministic.
