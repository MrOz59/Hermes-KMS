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
#include <poll.h>
#include <linux/udmabuf.h>
#include <linux/dma-buf.h>
#include <linux/memfd.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <drm/hermes_kms_drm.h>

#include "../hermes_session.h"

#define W 640u
#define H 480u
#define PITCH (W * 4u)
#define CURSOR_W 256u
#define CURSOR_H 256u
#define CURSOR_PITCH (CURSOR_W * 4u)
#define MAGIC 0xA5A5C3C3u

static int open_hermes_card(void)
{
	/* DRM primary minors occupy 0..63; do not silently miss busy hosts. */
	for (int i = 0; i < 64; i++) {
		char p[64];
		snprintf(p, sizeof p, "/dev/dri/card%d", i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		int ok = hermes_session_require_driver(fd) == 0;
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
	if (mfd < 0 || ftruncate(mfd, (off_t)size) < 0 ||
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

	int card = open_hermes_card();
	if (card < 0) { fprintf(stderr, "FAIL: no hermes-kms card\n"); return 1; }
	struct drm_hermes_kms_set_output output = {
		.enabled = 1,
		.width = W,
		.height = H,
		.refresh_hz = 60,
	};
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0) {
		perror("FAIL SET_OUTPUT"); return 1;
	}

	if (drmSetMaster(card)) {
		fprintf(stderr, "FAIL: drmSetMaster failed: %s\n", strerror(errno));
		return 1;
	}

	uint32_t handle = 0;
	if (drmPrimeFDToHandle(card, src, &handle)) {
		perror("FAIL drmPrimeFDToHandle (import into hermes-kms)"); return 1;
	}
	printf("PASS: imported it into hermes-kms (handle %u)\n", handle);

	/*
	 * mode_config.min_width/min_height constrain every framebuffer, not only
	 * display modes.  A mode-sized minimum used to make DRM reject Mutter's
	 * 256x256 ARGB cursor before the driver's fb_create callback.  Exercise the
	 * exact AddFB2 shape here so that regression is caught without GNOME too.
	 */
	{
		uint32_t cursor_handles[4] = { handle };
		uint32_t cursor_pitches[4] = { CURSOR_PITCH };
		uint32_t cursor_offsets[4] = { 0 };
		uint32_t cursor_fb = 0;

		if (drmModeAddFB2(card, CURSOR_W, CURSOR_H,
				  DRM_FORMAT_ARGB8888, cursor_handles,
				  cursor_pitches, cursor_offsets, &cursor_fb, 0)) {
			perror("FAIL small ARGB drmModeAddFB2");
			return 1;
		}
		printf("PASS: small ARGB framebuffer %u accepted for cursor use\n",
		       cursor_fb);
		if (drmModeRmFB(card, cursor_fb)) {
			perror("FAIL remove small ARGB framebuffer");
			return 1;
		}
	}

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
	if (!conn) { fprintf(stderr, "FAIL: no connected connector after SET_OUTPUT\n"); return 1; }

	drmModeEncoder *enc = conn->count_encoders > 0 ?
		drmModeGetEncoder(card, conn->encoders[0]) : NULL;
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

	struct hermes_session_credentials credentials;
	int rnd;

	memset(&credentials, 0, sizeof(credentials));
	if (hermes_session_get_owner_token(card, &credentials) < 0) {
		perror("FAIL generic session capability handoff");
		hermes_session_forget(&credentials);
		return 1;
	}
	rnd = hermes_session_open_bound_render_credentials(&credentials, NULL, 0,
							     NULL);
	hermes_session_forget(&credentials);
	if (rnd < 0) {
		perror("FAIL: no matching bound hermes-kms render node");
		return 1;
	}
	struct drm_hermes_kms_acquire_frame f;
	memset(&f, 0, sizeof f);
	for (unsigned int i = 0; i < 4; i++)
		f.dma_buf_fd[i] = -1;
	f.sync_file_fd = -1;
	f.flags = HERMES_KMS_FRAME_REQUEST_DMABUF |
		  HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
	if (ioctl(rnd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &f) < 0) {
		perror("FAIL ACQUIRE_FRAME"); return 1;
	}
	if (!(f.flags & HERMES_KMS_FRAME_DMABUF_VALID) || f.dma_buf_fd[0] < 0) {
		fprintf(stderr, "FAIL: no DMA-BUF returned\n"); return 1;
	}
	if (f.width != W || f.height != H || f.format != DRM_FORMAT_XRGB8888 ||
	    f.plane_count != 1 || f.pitch[0] != PITCH || f.offset[0] != 0) {
		fprintf(stderr,
			"FAIL: wrong frame layout: %ux%u format=0x%08x planes=%u pitch=%u offset=%u\n",
			f.width, f.height, f.format, f.plane_count, f.pitch[0], f.offset[0]);
		return 1;
	}
	if (!(f.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) || f.sync_file_fd < 0) {
		fprintf(stderr, "FAIL: no producer sync_file returned\n");
		return 1;
	}
	if (hermes_sync_file_wait(f.sync_file_fd, 2000) < 0) {
		fprintf(stderr, "FAIL: producer fence wait failed: %s\n",
			strerror(errno));
		return 1;
	}
	printf("PASS: ACQUIRE_FRAME returned a DMA-BUF\n");

	size_t returned_size = (size_t)f.offset[0] + (size_t)f.pitch[0] * f.height;
	void *g = mmap(NULL, returned_size, PROT_READ, MAP_SHARED, f.dma_buf_fd[0], 0);
	if (g == MAP_FAILED) {
		printf("FAIL: cannot mmap the returned DMA-BUF (%s)\n", strerror(errno));
		printf("      -> this is the re-exported pageless object\n");
		return 1;
	}
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
	if (ioctl(f.dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &sync) < 0) {
		perror("FAIL DMA_BUF_IOCTL_SYNC START");
		return 1;
	}
	uint32_t first = *(uint32_t *)((uint8_t *)g + f.offset[0]);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	if (ioctl(f.dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &sync) < 0) {
		perror("FAIL DMA_BUF_IOCTL_SYNC END");
		return 1;
	}
	if (first == MAGIC) {
		printf("PASS: returned buffer holds the source data (0x%08X)\n", first);
		printf("\nRESULT: PASS - the imported buffer was handed back intact\n");
		return 0;
	}
	printf("FAIL: returned buffer reads 0x%08X, expected 0x%08X\n", first, MAGIC);
	printf("\nRESULT: FAIL - wrong buffer handed back\n");
	return 1;
}
