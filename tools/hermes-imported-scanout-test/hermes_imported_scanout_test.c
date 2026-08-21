// SPDX-License-Identifier: GPL-2.0
// hermes-imported-scanout-test: scan out of a buffer this driver did not
// allocate, then check ACQUIRE_FRAME hands back something usable.
//
// A compositor that renders on the real GPU can import that buffer into this
// device and scan out of it directly rather than drawing into a dumb buffer we
// allocated - Mutter does this where KWin does not. gem_prime_import then gives
// us a shmem object with import_attach set and no shmem file behind it, and
// re-exporting that object produces a DMA-BUF whose pages cannot be pinned.
//
// The consumer only finds out when it attaches, far from the cause:
// drm_gem_map_attach() -> drm_gem_shmem_pin_locked() warns on
// drm_gem_is_imported(), drm_gem_get_pages() rejects the NULL filp, and the
// user sees EGL_BAD_ALLOC out of eglCreateImage().
//
// This builds that exact situation without needing a GPU or a compositor: a
// DMA-BUF from a memfd via udmabuf, PRIME-imported here, made the scanout
// framebuffer. It then acquires a frame and reads the buffer back, so a driver
// that re-exports the pageless object fails visibly instead of handing the
// problem downstream.

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/udmabuf.h>
#include <linux/memfd.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <drm/hermes_kms_drm.h>

#define W 640u
#define H 480u
#define PITCH (W * 4u)
#define MAGIC 0xA5A5C3C3u

static int open_hermes(const char *what)
{
	for (int i = 0; i < 8; i++) {
		char p[64];
		snprintf(p, sizeof p, "/dev/dri/%s%d", what,
			 strcmp(what, "card") == 0 ? i : 128 + i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		drmVersionPtr v = drmGetVersion(fd);
		int ok = v && v->name && !strcmp(v->name, "hermes-kms");
		if (v) drmFreeVersion(v);
		if (ok) return fd;
		close(fd);
	}
	return -1;
}

int main(void)
{
	size_t page = (size_t) sysconf(_SC_PAGESIZE);
	size_t size = ((size_t) PITCH * H + page - 1) & ~(page - 1);

	int mfd = memfd_create("fb", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (mfd < 0 || ftruncate(mfd, size) < 0 ||
	    fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
		perror("FAIL memfd"); return 1;
	}
	uint32_t *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (m == MAP_FAILED) { perror("FAIL mmap memfd"); return 1; }
	for (size_t i = 0; i < size / 4; i++) m[i] = MAGIC;

	int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udev < 0) { perror("FAIL /dev/udmabuf"); return 1; }
	struct udmabuf_create c = { .memfd = (uint32_t) mfd, .size = size };
	int src = ioctl(udev, UDMABUF_CREATE, &c);
	if (src < 0) { perror("FAIL UDMABUF_CREATE"); return 1; }
	printf("PASS: source DMA-BUF of %zu bytes filled with 0x%08X\n", size, MAGIC);

	int card = open_hermes("card");
	if (card < 0) { fprintf(stderr, "FAIL: no hermes-kms card\n"); return 1; }

	if (drmSetMaster(card))
		fprintf(stderr, "note: drmSetMaster failed (%s) - modeset may be refused\n",
			strerror(errno));

	uint32_t handle = 0;
	if (drmPrimeFDToHandle(card, src, &handle)) {
		perror("FAIL drmPrimeFDToHandle (import into hermes-kms)"); return 1;
	}
	printf("PASS: imported it into hermes-kms (handle %u)\n", handle);

	uint32_t handles[4] = { handle }, pitches[4] = { PITCH }, offsets[4] = { 0 };
	uint32_t fb = 0;
	if (drmModeAddFB2(card, W, H, DRM_FORMAT_XRGB8888, handles, pitches,
			  offsets, &fb, 0)) {
		perror("FAIL drmModeAddFB2"); return 1;
	}
	printf("PASS: framebuffer %u from the imported object\n", fb);

	drmModeRes *res = drmModeGetResources(card);
	if (!res) { fprintf(stderr, "FAIL: no resources\n"); return 1; }
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnector *c2 = drmModeGetConnector(card, res->connectors[i]);
		if (c2 && c2->connection == DRM_MODE_CONNECTED && c2->count_modes)
			conn = c2;
		else if (c2) drmModeFreeConnector(c2);
	}
	if (!conn) { fprintf(stderr, "FAIL: no connected connector (hold the output first)\n"); return 1; }

	drmModeEncoder *enc = drmModeGetEncoder(card, conn->encoders[0]);
	uint32_t crtc = enc ? enc->crtc_id : 0;
	if (!crtc && res->count_crtcs) crtc = res->crtcs[0];
	drmModeModeInfo *mode = NULL;
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].hdisplay == W && conn->modes[i].vdisplay == H) {
			mode = &conn->modes[i];
			break;
		}
	if (!mode) { fprintf(stderr, "FAIL: connector has no %ux%u mode\n", W, H); return 1; }
	printf("      using connector %u, crtc %u, mode %s\n",
	       conn->connector_id, crtc, mode->name);
	if (drmModeSetCrtc(card, crtc, fb, 0, 0, &conn->connector_id, 1, mode)) {
		perror("FAIL drmModeSetCrtc"); return 1;
	}
	printf("PASS: scanning out of the imported framebuffer\n");
	usleep(300000);

	int rnd = open_hermes("renderD");
	if (rnd < 0) { fprintf(stderr, "FAIL: no hermes-kms render node\n"); return 1; }
	struct drm_hermes_kms_acquire_frame f;
	memset(&f, 0, sizeof f);
	f.flags = HERMES_KMS_FRAME_REQUEST_DMABUF;
	if (ioctl(rnd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &f) < 0) {
		perror("FAIL ACQUIRE_FRAME"); return 1;
	}
	if (!(f.flags & HERMES_KMS_FRAME_DMABUF_VALID) || f.dma_buf_fd[0] < 0) {
		fprintf(stderr, "FAIL: no DMA-BUF returned\n"); return 1;
	}
	printf("PASS: ACQUIRE_FRAME returned a DMA-BUF\n");

	void *g = mmap(NULL, size, PROT_READ, MAP_SHARED, f.dma_buf_fd[0], 0);
	if (g == MAP_FAILED) {
		printf("FAIL: cannot mmap the returned DMA-BUF (%s)\n", strerror(errno));
		printf("      -> this is the re-exported pageless object\n");
		return 1;
	}
	uint32_t first = ((uint32_t *) g)[0];
	if (first == MAGIC) {
		printf("PASS: returned buffer holds the source data (0x%08X)\n", first);
		printf("\nRESULT: PASS - the imported buffer was handed back intact\n");
		return 0;
	}
	printf("FAIL: returned buffer reads 0x%08X, expected 0x%08X\n", first, MAGIC);
	printf("\nRESULT: FAIL - wrong buffer handed back\n");
	return 1;
}
