// SPDX-License-Identifier: GPL-2.0
/* Disposable-VM integration test; never run on a production display server.
 * Pass the primary node of an idle Hermes card loaded with hdr_enable=1.
 * The owner/DRM master and the authorized render-node consumer are separate.
 */
#define _GNU_SOURCE
#include "../tools/hermes_session.h"
#include <drm/drm_fourcc.h>
#include <drm/hermes_kms_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define CHECK(x)                                                               \
	do {                                                                   \
		if (!(x)) {                                                    \
			fprintf(stderr, "FAIL line %d: %s (errno=%d)\n",       \
				__LINE__, #x, errno);                          \
			exit(1);                                               \
		}                                                              \
	} while (0)

static uint32_t property(int fd, uint32_t object, uint32_t type,
			 const char *name)
{
	drmModeObjectProperties *props =
	    drmModeObjectGetProperties(fd, object, type);
	uint32_t id = 0;
	CHECK(props);
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		CHECK(p);
		if (!strcmp(p->name, name))
			id = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	CHECK(id);
	return id;
}

static int color_commit(int fd, uint32_t connector, uint32_t cp, uint32_t hp,
			uint32_t colorspace, uint32_t blob, uint32_t flags)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret;
	CHECK(req);
	CHECK(drmModeAtomicAddProperty(req, connector, cp, colorspace) >= 0);
	CHECK(drmModeAtomicAddProperty(req, connector, hp, blob) >= 0);
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static struct drm_hermes_kms_acquire_frame2 acquire(int fd)
{
	struct drm_hermes_kms_acquire_frame2 f = {0};
	int ret;
	for (int i = 0; i < 20; i++) {
		memset(&f, 0, sizeof(f));
		f.frame.flags = HERMES_KMS_FRAME_REQUEST_DMABUF |
				HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
		ret = ioctl(fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, &f);
		if (!ret || errno != ESTALE)
			break;
	}
	CHECK(!ret);
	CHECK(f.frame.flags & HERMES_KMS_FRAME_DMABUF_VALID);
	CHECK(f.frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID);
	CHECK(f.frame.plane_count == 1 && f.frame.dma_buf_fd[0] >= 0);
	CHECK(hermes_sync_file_wait(f.frame.sync_file_fd, 2000) == 0);
	CHECK(f.color.flags & HERMES_KMS_COLOR_VALID);
	CHECK(f.color.flags & HERMES_KMS_COLOR_RGB_FULL_RANGE);
	CHECK(!f.color.reserved[0] && !f.color.reserved[1]);
	close(f.frame.dma_buf_fd[0]);
	close(f.frame.sync_file_fd);
	return f;
}

struct race_context {
	int fd;
	uint32_t pq_fb, sdr_fb;
	atomic_int stop;
	unsigned int captures;
};

static void *capture_race(void *opaque)
{
	struct race_context *ctx = opaque;
	while (!atomic_load(&ctx->stop)) {
		struct drm_hermes_kms_acquire_frame2 f = acquire(ctx->fd);
		if (f.color.flags & HERMES_KMS_COLOR_HDR_VALID) {
			CHECK(f.frame.framebuffer_id == ctx->pq_fb);
			CHECK(f.color.colorspace ==
			      HERMES_KMS_COLORSPACE_BT2020_RGB);
			CHECK(f.color.hdr.hdmi_metadata_type1.eotf ==
			      HERMES_KMS_EOTF_PQ);
		} else {
			CHECK(f.frame.framebuffer_id == ctx->sdr_fb);
			CHECK(f.color.colorspace ==
			      HERMES_KMS_COLORSPACE_DEFAULT);
		}
		ctx->captures++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int card, render, unbound;
	uint32_t fb, cp, hp, blob, bad;
	struct drm_hermes_kms_caps caps = {0};
	struct drm_hermes_kms_identity id = {0};
	struct drm_hermes_kms_set_output output = {
	    .enabled = 1,
	    .width = 640,
	    .height = 480,
	    .refresh_hz = 60,
	};
	struct drm_mode_create_dumb dumb = {
	    .width = 640, .height = 480, .bpp = 32};
	struct hermes_session_credentials credentials = {0};
	struct drm_hermes_kms_acquire_frame2 f, previous;
	struct hdr_output_metadata hdr = {0};
	drmModeConnector *connector;
	drmModeModeInfo *mode = NULL;
	char *render_path;
	CHECK(argc == 2);
	card = open(argv[1], O_RDWR | O_CLOEXEC);
	CHECK(card >= 0);
	CHECK(hermes_session_require_driver(card) == 0);
	CHECK(ioctl(card, DRM_IOCTL_HERMES_KMS_GET_CAPS, &caps) == 0);
	CHECK(caps.flags & HERMES_KMS_CAP_FRAME_COLOR);
	CHECK(ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) == 0);
	CHECK(ioctl(card, DRM_IOCTL_HERMES_KMS_GET_IDENTITY, &id) == 0);
	CHECK(drmSetMaster(card) == 0);
	CHECK(drmSetClientCap(card, DRM_CLIENT_CAP_ATOMIC, 1) == 0);
	CHECK(ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) == 0);
	uint32_t handles[4] = {dumb.handle}, pitches[4] = {dumb.pitch},
		 offsets[4] = {0};
	CHECK(drmModeAddFB2(card, 640, 480, DRM_FORMAT_XRGB2101010, handles,
			    pitches, offsets, &fb, 0) == 0);
	connector = drmModeGetConnector(card, id.connector_id);
	CHECK(connector);
	for (int i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].hdisplay == 640 &&
		    connector->modes[i].vdisplay == 480)
			mode = &connector->modes[i];
	CHECK(mode);
	CHECK(drmModeSetCrtc(card, id.crtc_id, fb, 0, 0, &id.connector_id, 1,
			     mode) == 0);
	CHECK(hermes_session_get_owner_token(card, &credentials) == 0);
	render = hermes_session_open_bound_render_credentials(&credentials,
							      NULL, 0, NULL);
	hermes_session_forget(&credentials);
	CHECK(render >= 0);
	render_path = drmGetRenderDeviceNameFromFd(card);
	CHECK(render_path);
	unbound = open(render_path, O_RDWR | O_CLOEXEC);
	free(render_path);
	CHECK(unbound >= 0);
	memset(&f, 0, sizeof(f));
	CHECK(ioctl(unbound, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, &f) == -1 &&
	      errno == EACCES);
	close(unbound);
	previous = acquire(render);
	CHECK(previous.frame.format == DRM_FORMAT_XRGB2101010);
	CHECK(!(previous.color.flags & HERMES_KMS_COLOR_HDR_VALID));
	CHECK(previous.color.colorspace == 0); /* Ten-bit SDR stays SDR. */
	cp = property(card, id.connector_id, DRM_MODE_OBJECT_CONNECTOR,
		      "Colorspace");
	hp = property(card, id.connector_id, DRM_MODE_OBJECT_CONNECTOR,
		      "HDR_OUTPUT_METADATA");
	hdr.hdmi_metadata_type1.eotf = 2;
	hdr.hdmi_metadata_type1.display_primaries[0].x = 34000;
	hdr.hdmi_metadata_type1.display_primaries[0].y = 16000;
	hdr.hdmi_metadata_type1.white_point.x = 15635;
	hdr.hdmi_metadata_type1.white_point.y = 16450;
	hdr.hdmi_metadata_type1.max_display_mastering_luminance = 1000;
	hdr.hdmi_metadata_type1.min_display_mastering_luminance = 50;
	hdr.hdmi_metadata_type1.max_cll = 900;
	hdr.hdmi_metadata_type1.max_fall = 400;
	CHECK(drmModeCreatePropertyBlob(card, &hdr, sizeof(hdr), &blob) == 0);
	/* DRM_MODE_COLORIMETRY_BT2020_RGB = 9, a stable property ABI value. */
	CHECK(color_commit(card, id.connector_id, cp, hp, 9, blob,
			   DRM_MODE_ATOMIC_TEST_ONLY) == 0);
	f = acquire(render);
	CHECK(f.frame.sequence == previous.frame.sequence &&
	      f.color.flags == previous.color.flags);
	CHECK(color_commit(card, id.connector_id, cp, hp, 9, blob, 0) == 0);
	struct drm_hermes_kms_wait_frame wait = {.after_sequence =
						     previous.frame.sequence};
	CHECK(ioctl(render, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) == 0);
	f = acquire(render);
	CHECK(f.frame.sequence > previous.frame.sequence &&
	      f.frame.framebuffer_id == fb);
	CHECK(!(f.frame.flags & HERMES_KMS_FRAME_DAMAGE_VALID));
	CHECK(f.color.colorspace == 9 &&
	      (f.color.flags & HERMES_KMS_COLOR_HDR_VALID));
	CHECK(!memcmp(&f.color.hdr, &hdr, sizeof(hdr)));
	previous = f;
	puts("PASS: ten-bit SDR, TEST_ONLY, PQ metadata-only commit, wait and "
	     "capture");
	/* Invalid blobs must fail TEST_ONLY and leave published state
	 * untouched. */
	CHECK(drmModeCreatePropertyBlob(card, &hdr, sizeof(hdr) - 1, &bad) ==
	      0);
	CHECK(color_commit(card, id.connector_id, cp, hp, 9, bad,
			   DRM_MODE_ATOMIC_TEST_ONLY) < 0);
	CHECK(drmModeDestroyPropertyBlob(card, bad) == 0);
	hdr.hdmi_metadata_type1.eotf = 3; /* HLG is not advertised. */
	CHECK(drmModeCreatePropertyBlob(card, &hdr, sizeof(hdr), &bad) == 0);
	CHECK(color_commit(card, id.connector_id, cp, hp, 9, bad,
			   DRM_MODE_ATOMIC_TEST_ONLY) < 0);
	CHECK(drmModeDestroyPropertyBlob(card, bad) == 0);
	CHECK(color_commit(card, id.connector_id, cp, hp, 0, blob,
			   DRM_MODE_ATOMIC_TEST_ONLY) < 0);
	f = acquire(render);
	CHECK(f.frame.sequence == previous.frame.sequence);
	CHECK(!memcmp(&f.color, &previous.color, sizeof(f.color)));
	memset(&f, 0, sizeof(f));
	f.color.reserved[0] = 1;
	CHECK(ioctl(render, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, &f) == -1 &&
	      errno == EINVAL);
	puts("PASS: malformed metadata, unsupported EOTF, inconsistent PQ and "
	     "reserved input rejected");
	/* Repeated nonblocking metadata-only transitions exercise snapshot
	 * lifetime. */
	for (int i = 0; i < 100; i++) {
		const int pq = i % 2;
		int ret;
		for (int attempt = 0;; attempt++) {
			ret = color_commit(card, id.connector_id, cp, hp,
					   pq ? 9 : 0, pq ? blob : 0,
					   DRM_MODE_ATOMIC_NONBLOCK);
			if (!ret || errno != EBUSY || attempt == 100)
				break;
			usleep(1000);
		}
		CHECK(ret == 0);
		wait = (struct drm_hermes_kms_wait_frame){
		    .after_sequence = previous.frame.sequence,
		    .timeout_ms = 2000};
		CHECK(ioctl(render, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) ==
		      0);
		f = acquire(render);
		CHECK(f.color.colorspace == (pq ? 9U : 0U));
		CHECK(!!(f.color.flags & HERMES_KMS_COLOR_HDR_VALID) == pq);
		if (!pq)
			CHECK(!memcmp(&f.color.hdr,
				      &(struct hdr_output_metadata){0},
				      sizeof(hdr)));
		previous = f;
	}
	puts("PASS: repeated nonblocking SDR/HDR transitions clear stale "
	     "metadata");
	/* Alternate paired framebuffer/colour states while a consumer exports
	 * fds. A later live connector-state read can attach SDR metadata to the
	 * PQ fb.
	 */
	uint32_t sdr_fb;
	CHECK(drmModeAddFB2(card, 640, 480, DRM_FORMAT_XRGB8888, handles,
			    pitches, offsets, &sdr_fb, 0) == 0);
	uint32_t fp =
	    property(card, id.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
	struct race_context race = {
	    .fd = render, .pq_fb = fb, .sdr_fb = sdr_fb};
	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, capture_race, &race) == 0);
	for (int i = 0; i < 100; i++) {
		const int pq = i % 2;
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		CHECK(req);
		CHECK(drmModeAtomicAddProperty(req, id.connector_id, cp,
					       pq ? 9 : 0) >= 0);
		CHECK(drmModeAtomicAddProperty(req, id.connector_id, hp,
					       pq ? blob : 0) >= 0);
		CHECK(drmModeAtomicAddProperty(req, id.plane_id, fp,
					       pq ? fb : sdr_fb) >= 0);
		int ret;
		for (int attempt = 0;; attempt++) {
			ret = drmModeAtomicCommit(
			    card, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
			if (!ret || errno != EBUSY || attempt == 100)
				break;
			usleep(1000);
		}
		CHECK(ret == 0);
		drmModeAtomicFree(req);
	}
	atomic_store(&race.stop, 1);
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(race.captures > 0);
	printf("PASS: %u concurrent acquisitions preserve framebuffer/colour "
	       "pairing\n",
	       race.captures);
	/* Drain the final nonblocking commit before legacy comparison. */
	for (int i = 0; i < 100; i++) {
		f = acquire(render);
		if (f.frame.framebuffer_id == fb)
			break;
		usleep(1000);
	}
	CHECK(f.frame.framebuffer_id == fb);
	/* Old ioctl still works, and revocation protects the new ioctl too. */
	struct drm_hermes_kms_acquire_frame legacy = {0};
	CHECK(ioctl(render, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &legacy) == 0);
	CHECK(legacy.framebuffer_id == fb);
	struct drm_hermes_kms_session_access revoke = {
	    .operation = HERMES_KMS_SESSION_ACCESS_REVOKE_BINDINGS};
	CHECK(ioctl(card, DRM_IOCTL_HERMES_KMS_SESSION_ACCESS, &revoke) == 0);
	memset(&f, 0, sizeof(f));
	CHECK(ioctl(render, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, &f) == -1 &&
	      errno == EACCES);
	CHECK(drmModeSetCrtc(card, id.crtc_id, 0, 0, 0, NULL, 0, NULL) == 0);
	CHECK(drmModeDestroyPropertyBlob(card, blob) == 0);
	CHECK(drmModeRmFB(card, fb) == 0);
	CHECK(drmModeRmFB(card, sdr_fb) == 0);
	struct drm_mode_destroy_dumb destroy = {.handle = dumb.handle};
	CHECK(ioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) == 0);
	drmModeFreeConnector(connector);
	close(render);
	close(card);
	puts("HDR capture: PASS");
	return 0;
}
