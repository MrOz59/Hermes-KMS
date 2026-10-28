# Upstream references and integration boundaries

Reviewed on 2026-10-25 alongside `main` (driver 0.4.0, UAPI v14). These references
explain the external APIs used by the driver and tools. They are not evidence
that every GPU/compositor combination works. The repository's
[UAPI header](../include/uapi/drm/hermes_kms_drm.h), [HDR contract](hdr-capture.md)
and [tests](../tests) define the Hermes-KMS-specific behavior.

## Kernel and buffer contracts

| Reference | Relevance to Hermes-KMS |
| --- | --- |
| [DRM userland interfaces](https://docs.kernel.org/gpu/drm-uapi.html) | Primary nodes, DRM master, render-node permissions and hot-unplug semantics. Capture uses the render node; KMS ownership belongs to the compositor. |
| [DRM/KMS](https://docs.kernel.org/gpu/drm-kms.html) | Atomic state lifetime, planes, formats/modifiers, connector properties and colour management. |
| [Linux `drm_mode.h`](https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_mode.h) | Public `hdr_output_metadata` representation and mastering metadata units used by UAPI v14. Use the definitions matching the target headers. |
| [DMA-BUF sharing and synchronization](https://docs.kernel.org/driver-api/dma-buf.html) | Export/import, `dma_resv`, producer fences and CPU-access synchronization. An exported fd does not imply completed rendering or correct importer sampling. |
| [Configfs](https://docs.kernel.org/filesystems/configfs.html) | Userspace-owned kernel object lifetimes through mkdir, attributes and rmdir. |
| [VKMS runtime configuration](https://docs.kernel.org/gpu/vkms.html#configuring-with-configfs) | Upstream precedent for creating virtual DRM cards at runtime. Hermes-KMS has its own [attribute schema](configfs.md); it is not a drop-in VKMS configfs ABI. |

An accepted format/modifier describes a layout, not a guarantee that a particular
GPU can import it without copying. The compositor allocator and capture consumer
must agree on that layout and its synchronization.

## Consumer import and encoding

| Reference | Relevance and limit |
| --- | --- |
| [VA-API core API](https://intel.github.io/libva/group__api__core.html) | Surface creation used by the VAAPI preflight; the tool supplies a DRM PRIME descriptor. Surface acceptance is narrower than complete encoding validation. |
| [EGL DMA-BUF import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt) | Fourcc, plane fd, offset and pitch attributes for creating an EGLImage. |
| [EGL DMA-BUF modifier import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt) | Explicit layout modifiers and queries for supported formats/modifiers. A listed or accepted import must still be checked against actual pixels. |
| [NVIDIA encode/decode support matrix](https://developer.nvidia.com/video-encode-decode-support-matrix) | Hardware codec and ten-bit encode capabilities. NVENC support does not establish correct DMA-BUF/EGL import or zero-copy for a Hermes-KMS buffer. |
| [Hermes HDR integration](https://github.com/MrOz59/Hermes/blob/main/docs/hermes_kms_hdr.md) | Reference consumer's setup, conversion/encoding paths, fallback behavior and end-to-end validation gates. It is a separate application and can evolve independently. |

The historical NVIDIA import result is scoped to the tested driver/GPU in the
[EGL checker guide](../tools/hermes-egl-import-check/README.md), not a statement
about every NVIDIA release. The
[experimental NVIDIA branch](https://github.com/MrOz59/Hermes-KMS/tree/experimental/nvidia-zero-copy)
contains further diagnostics; see [roadmap.md](roadmap.md#separate-experiments).

## Seats and service policy

The [seatd manual](https://man.archlinux.org/man/seatd.1.en) documents the
compile-time socket location and `SEATD_VTBOUND=0`. Hermes-KMS uses a separate
mount namespace per broker to expose a private socket and sets that environment
in the [packaged systemd unit](../packaging/systemd/hermes-kms-seatd@.service).
A custom launch must preserve both arrangements. This separates broker activation
state; it does not create a sandbox for mutually untrusted users.

## Installation

- [bootc image model](https://bootc-dev.github.io/bootc/) and
  [`bootc switch`](https://bootc.dev/bootc/man/bootc-switch.8.html): a derived
  system image carries its kernel/module tree and is deployed for a subsequent
  boot. The repository's [Bazzite Containerfile](../packaging/bazzite/Containerfile)
  builds against that image's kernel, not the build host's kernel.
- [Kernel module signing](https://docs.kernel.org/admin-guide/module-signing.html):
  signing a module and trusting its certificate are separate requirements.
  Hermes-KMS accepts build secrets for signing; certificate enrollment remains
  part of the target distribution's Secure Boot setup.

The shipped Containerfile is a Bazzite/Fedora integration example. It does not
establish support for every immutable distribution, base-image tag or kernel.
Rebuild the derived image when its kernel changes and validate the resulting
module on the intended system.
