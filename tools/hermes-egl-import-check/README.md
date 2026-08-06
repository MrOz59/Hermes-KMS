# hermes-egl-import-check / pitch_detect

Diagnostic consumers for the NVENC/EGL side of the roadmap item
"NVENC/AMF DMA-BUF import validation".

## hermes_egl_import_check.c

Validates the NVENC-style consumer chain for a live Hermes-KMS frame:

```
ACQUIRE_FRAME (DMA-BUF) -> eglCreateImage(EGL_LINUX_DMA_BUF_EXT)
  -> texture bind (OES, falling back to glEGLImageTargetTexStorageEXT)
  -> FBO readback (proves sampling)
  -> GL blit into an own texture -> cuGraphicsGLRegisterImage
     (mirrors the Sunshine/Hermes CUDA converter flow)
```

Build:

```bash
cc -O2 -Wall -DHAVE_CUDA -I ../../include/uapi -I/usr/include/libdrm \
  -o hermes-egl-import-check hermes_egl_import_check.c \
  -ldrm -lgbm -lEGL -lGL -lcuda
```

## pitch_detect.c

Compares the GPU's view of an imported frame against the CPU view of the
same DMA-BUF under several hypotheses (declared pitch, width*4, vertical
flip, R/B swap, brute-forced pitch scan, drift-filtered rows).

## Known finding (2026-08, NVIDIA 595.84, RTX 5060 Ti)

The NVIDIA proprietary driver imports these system-memory DMA-BUFs via
`EGL_EXT_image_dma_buf_import` (with the TexStorageEXT bind), but the
sampled content is only correct for roughly the first contiguous ~2 MB of
the buffer; beyond that the GPU reads the wrong pages while the CPU view
(`mmap`) of the very same DMA-BUF is pixel-perfect. Visible as diagonal
stripe corruption at larger modes. The Hermes NVIDIA fork therefore uses a
CPU-copy capture path for NVENC sessions until this is resolved.
