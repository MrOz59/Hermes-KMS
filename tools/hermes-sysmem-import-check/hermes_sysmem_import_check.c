// SPDX-License-Identifier: GPL-2.0
// hermes-sysmem-import-check: can this GPU import a plain system-memory
// DMA-BUF at all?
//
// This is the Hermes-free control for hermes-egl-import-check. It builds a
// DMA-BUF out of a memfd through udmabuf - ordinary system RAM, with no GPU,
// no virtual display driver and no compositor anywhere in the picture - then
// asks EGL to import it with the geometry, format and modifier a Hermes-KMS
// scanout frame carries.
//
// The point is to split a failing import into two very different causes:
//
//   FAIL here -> the GPU stack cannot import foreign system-memory DMA-BUFs
//                for that geometry. Hermes-KMS is not involved and the same
//                failure would hit any producer of such buffers.
//   PASS here, but hermes-egl-import-check fails -> something specific to the
//                buffers Hermes-KMS exports, and the driver is worth looking at.
//
// Usage: hermes-sysmem-import-check [WIDTH] [HEIGHT] [PITCH_BYTES]
//
// The optional pitch is what makes this useful as a self-test. radeonsi wants
// a linear surface's pitch aligned to 256 bytes (64 pixels at 4 bytes each) and
// refuses the import otherwise, which is reported as EGL_BAD_ALLOC - the same
// error a genuinely broken import produces. Passing a deliberately misaligned
// pitch should therefore FAIL on a healthy system:
//
//   hermes-sysmem-import-check 854 480 3416   -> expected FAIL (854 % 64 = 22)
//   hermes-sysmem-import-check 854 480 3584   -> expected PASS (896 % 64 = 0)
//
// Hermes-KMS pads its backing pitch to 256 bytes precisely to stay on the
// passing side of that rule, so the first line is not a bug report - it is how
// you confirm the tool can see the failure it is looking for.

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#include <linux/udmabuf.h>
#include <linux/memfd.h>
#include <xf86drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#define FOURCC_XR24 0x34325258u

static int open_real_gpu(char *name, size_t n)
{
	for (int i = 128; i < 140; i++) {
		char p[64];
		snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr v = drmGetVersion(fd);
		int ok = v && v->name && strcmp(v->name, "hermes-kms") != 0 &&
			 strcmp(v->name, "evdi") != 0;
		if (v) {
			if (ok) snprintf(name, n, "%s (%s)", v->name, p);
			drmFreeVersion(v);
		}
		if (ok)
			return fd;
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	unsigned width  = argc > 1 ? (unsigned) atoi(argv[1]) : 1600;
	unsigned height = argc > 2 ? (unsigned) atoi(argv[2]) : 1068;
	unsigned pitch  = argc > 3 ? (unsigned) atoi(argv[3]) : width * 4;
	size_t page = (size_t) sysconf(_SC_PAGESIZE);
	size_t size = ((size_t) pitch * height + page - 1) & ~(page - 1);

	printf("geometry : %ux%u  XR24  pitch=%u (%u px, %%64=%u)  size=%zu (pitch*h=%zu)\n",
	       width, height, pitch, pitch / 4, (pitch / 4) % 64, size,
	       (size_t) pitch * height);

	int mfd = memfd_create("udmabuf-test", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (mfd < 0) { perror("FAIL memfd_create"); return 1; }
	if (ftruncate(mfd, (off_t) size) < 0) { perror("FAIL ftruncate"); return 1; }
	if (fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) { perror("FAIL F_SEAL_SHRINK"); return 1; }

	int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udev < 0) {
		perror("FAIL open /dev/udmabuf");
		fprintf(stderr, "      (needs access to /dev/udmabuf - try as root)\n");
		return 1;
	}

	struct udmabuf_create create = { .memfd = (uint32_t) mfd, .flags = 0,
					 .offset = 0, .size = size };
	int dbuf = ioctl(udev, UDMABUF_CREATE, &create);
	if (dbuf < 0) { perror("FAIL UDMABUF_CREATE"); return 1; }
	printf("PASS     : created a %zu byte system-memory DMA-BUF via udmabuf\n", size);

	char gpu[128] = "?";
	int gpu_fd = open_real_gpu(gpu, sizeof gpu);
	if (gpu_fd < 0) { fprintf(stderr, "FAIL: no real GPU render node\n"); return 1; }
	printf("GPU      : %s\n", gpu);

	struct gbm_device *gbm = gbm_create_device(gpu_fd);
	if (!gbm) { fprintf(stderr, "FAIL gbm_create_device\n"); return 1; }
	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType) gbm);
	EGLint maj, min;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
		fprintf(stderr, "FAIL eglInitialize\n"); return 1;
	}
	const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
	int has_mod = ext && strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers");
	printf("EGL      : %d.%d, modifiers extension %s\n", maj, min,
	       has_mod ? "present" : "ABSENT");

	for (int pass = 0; pass < 2; pass++) {
		int with_mod = (pass == 0) && has_mod;
		EGLAttrib a[32];
		int i = 0;
		a[i++] = EGL_WIDTH;                     a[i++] = width;
		a[i++] = EGL_HEIGHT;                    a[i++] = height;
		a[i++] = EGL_LINUX_DRM_FOURCC_EXT;      a[i++] = FOURCC_XR24;
		a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;     a[i++] = dbuf;
		a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[i++] = 0;
		a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  a[i++] = pitch;
		if (with_mod) {
			a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; a[i++] = 0;
			a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; a[i++] = 0;
		}
		a[i++] = EGL_NONE;

		EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT,
					      EGL_LINUX_DMA_BUF_EXT, NULL, a);
		const char *label = with_mod ? "with explicit LINEAR modifier"
					     : "without modifier attributes";
		if (img == EGL_NO_IMAGE) {
			printf("FAIL     : eglCreateImage %s -> error=0x%x\n",
			       label, eglGetError());
		} else {
			printf("PASS     : eglCreateImage %s\n", label);
			eglDestroyImage(dpy, img);
		}
		if (!has_mod)
			break;
	}
	return 0;
}
