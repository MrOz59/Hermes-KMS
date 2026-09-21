// SPDX-License-Identifier: GPL-2.0
// hermes-hdr-metadata-test: commit HDR colour state the way a compositor does,
// then check ACQUIRE_FRAME2 hands the same values back to the capture consumer.
//
// Advertising HDR only tells a compositor the output can do it. What makes HDR
// deliverable is the other direction: the colour state the compositor commits
// has to reach whoever encodes the frame, because a PQ/BT.2020 frame described
// as nothing gets encoded as SDR and arrives at the client wrong. A physical
// sink is told out of band, in the AVI and Dynamic Range and Mastering
// InfoFrames. A virtual display has no transmitter, so the values travel with
// the frame instead - and that is what this exercises.
//
// It needs no GPU and no compositor: a dumb buffer, one atomic commit carrying
// Colorspace and HDR_OUTPUT_METADATA, and ACQUIRE_FRAME2 through the render node
// the session token hands out. Load the module with hdr_enable=1 first; without
// it the connector has no colour properties to commit and the test says so.
//
// The test checks ten-bit scanout, exact metadata delivery, legacy ioctl
// compatibility, a metadata-only transition, rejection of both descriptor ids,
// and clearing the state when the output is disabled.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <drm/hermes_kms_drm.h>

#include "../hermes_session.h"

#define W 640u
#define H 480u

/*
 * Distinctive mastering metadata: a real display's values would be plausible
 * enough to match something the driver might substitute on its own, and the
 * point here is to prove these exact bytes travelled. Primaries are BT.2020 in
 * the spec's 0.00002 units, and the luminance and CLL/FALL values are picked to
 * be individually recognisable in a failure message.
 */
static const struct hdr_metadata_infoframe expected_hdr = {
	.eotf = 2,		/* SMPTE ST2084 (PQ) */
	.metadata_type = 0,	/* Static Metadata Descriptor Type 1 */
	.display_primaries = {
		{ .x = 34000, .y = 16000 },
		{ .x = 13250, .y = 34500 },
		{ .x = 7500,  .y = 3000  },
	},
	.white_point = { .x = 15635, .y = 16450 },
	.max_display_mastering_luminance = 1000,
	.min_display_mastering_luminance = 5,
	.max_cll = 987,
	.max_fall = 654,
};

struct kms_target {
	uint32_t connector_id;
	uint32_t crtc_id;
	uint32_t plane_id;
	drmModeModeInfo mode;
	uint32_t prop_crtc_id;
	uint32_t prop_colorspace;
	uint32_t prop_hdr_metadata;
	uint32_t prop_crtc_active;
	uint32_t prop_crtc_mode_id;
	uint32_t prop_plane_fb_id;
	uint32_t prop_plane_crtc_id;
	uint32_t prop_plane_src_x;
	uint32_t prop_plane_src_y;
	uint32_t prop_plane_src_w;
	uint32_t prop_plane_src_h;
	uint32_t prop_plane_crtc_x;
	uint32_t prop_plane_crtc_y;
	uint32_t prop_plane_crtc_w;
	uint32_t prop_plane_crtc_h;
	uint64_t colorspace_bt2020_rgb;
	uint64_t colorspace_default;
};

static int open_hermes_card(void)
{
	/* DRM primary minors occupy 0..63; do not silently miss busy hosts. */
	for (int i = 0; i < 64; i++) {
		char path[64];
		int fd;

		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (hermes_session_require_driver(fd) == 0)
			return fd;
		close(fd);
	}
	return -1;
}

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
			  const char *name)
{
	drmModeObjectProperties *props =
		drmModeObjectGetProperties(fd, obj_id, obj_type);
	uint32_t id = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			id = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

/*
 * Colorspace is an enum property, so the value to commit is whatever the kernel
 * assigned to that name on this connector, not the DRM_MODE_COLORIMETRY_*
 * number. Look it up rather than assuming the two coincide.
 */
static bool find_enum_value(int fd, uint32_t prop_id, const char *name,
			    uint64_t *value)
{
	drmModePropertyRes *prop = drmModeGetProperty(fd, prop_id);
	bool found = false;

	if (!prop)
		return false;
	for (int i = 0; i < prop->count_enums && !found; i++) {
		if (strcmp(prop->enums[i].name, name) == 0) {
			*value = prop->enums[i].value;
			found = true;
		}
	}
	drmModeFreeProperty(prop);
	return found;
}

static bool discover(int fd, struct kms_target *t)
{
	drmModeRes *res = drmModeGetResources(fd);
	drmModeConnector *conn = NULL;
	drmModePlaneRes *planes;
	bool have_mode = false;

	if (!res) {
		fprintf(stderr, "FAIL: no DRM resources\n");
		return false;
	}
	for (int i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes)
			conn = c;
		else if (c)
			drmModeFreeConnector(c);
	}
	if (!conn) {
		fprintf(stderr, "FAIL: no connected connector after SET_OUTPUT\n");
		drmModeFreeResources(res);
		return false;
	}
	t->connector_id = conn->connector_id;
	for (int i = 0; i < conn->count_modes && !have_mode; i++) {
		if (conn->modes[i].hdisplay == W &&
		    conn->modes[i].vdisplay == H) {
			t->mode = conn->modes[i];
			have_mode = true;
		}
	}
	if (!have_mode) {
		fprintf(stderr, "FAIL: connector has no %ux%u mode\n", W, H);
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return false;
	}

	drmModeEncoder *enc = conn->count_encoders > 0 ?
		drmModeGetEncoder(fd, conn->encoders[0]) : NULL;
	t->crtc_id = enc ? enc->crtc_id : 0;
	if (enc)
		drmModeFreeEncoder(enc);
	if (!t->crtc_id && res->count_crtcs)
		t->crtc_id = res->crtcs[0];
	drmModeFreeConnector(conn);

	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		fprintf(stderr, "FAIL: no plane resources (universal planes?)\n");
		drmModeFreeResources(res);
		return false;
	}
	for (uint32_t i = 0; i < planes->count_planes && !t->plane_id; i++) {
		drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
		uint64_t type;

		if (!p)
			continue;
		if (p->possible_crtcs & 1u) {
			uint32_t type_prop = find_prop(fd, p->plane_id,
						       DRM_MODE_OBJECT_PLANE,
						       "type");
			drmModeObjectProperties *pp = drmModeObjectGetProperties(
				fd, p->plane_id, DRM_MODE_OBJECT_PLANE);

			type = DRM_PLANE_TYPE_OVERLAY;
			if (pp) {
				for (uint32_t j = 0; j < pp->count_props; j++)
					if (pp->props[j] == type_prop)
						type = pp->prop_values[j];
				drmModeFreeObjectProperties(pp);
			}
			if (type == DRM_PLANE_TYPE_PRIMARY)
				t->plane_id = p->plane_id;
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);
	if (!t->plane_id) {
		fprintf(stderr, "FAIL: no primary plane for crtc %u\n",
			t->crtc_id);
		return false;
	}

	t->prop_crtc_id = find_prop(fd, t->connector_id,
				    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
	t->prop_colorspace = find_prop(fd, t->connector_id,
				       DRM_MODE_OBJECT_CONNECTOR, "Colorspace");
	t->prop_hdr_metadata = find_prop(fd, t->connector_id,
					 DRM_MODE_OBJECT_CONNECTOR,
					 "HDR_OUTPUT_METADATA");
	t->prop_crtc_active = find_prop(fd, t->crtc_id, DRM_MODE_OBJECT_CRTC,
					"ACTIVE");
	t->prop_crtc_mode_id = find_prop(fd, t->crtc_id, DRM_MODE_OBJECT_CRTC,
					 "MODE_ID");
	t->prop_plane_fb_id = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					"FB_ID");
	t->prop_plane_crtc_id = find_prop(fd, t->plane_id,
					  DRM_MODE_OBJECT_PLANE, "CRTC_ID");
	t->prop_plane_src_x = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					"SRC_X");
	t->prop_plane_src_y = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					"SRC_Y");
	t->prop_plane_src_w = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					"SRC_W");
	t->prop_plane_src_h = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					"SRC_H");
	t->prop_plane_crtc_x = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					 "CRTC_X");
	t->prop_plane_crtc_y = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					 "CRTC_Y");
	t->prop_plane_crtc_w = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					 "CRTC_W");
	t->prop_plane_crtc_h = find_prop(fd, t->plane_id, DRM_MODE_OBJECT_PLANE,
					 "CRTC_H");

	if (!t->prop_colorspace || !t->prop_hdr_metadata) {
		fprintf(stderr,
			"FAIL: connector has no %s property; load the module with hdr_enable=1\n",
			t->prop_colorspace ? "HDR_OUTPUT_METADATA" :
					     "Colorspace");
		return false;
	}
	if (!t->prop_crtc_id || !t->prop_crtc_active || !t->prop_crtc_mode_id ||
	    !t->prop_plane_fb_id || !t->prop_plane_crtc_id ||
	    !t->prop_plane_src_w || !t->prop_plane_crtc_w) {
		fprintf(stderr, "FAIL: missing a standard atomic property\n");
		return false;
	}
	if (!find_enum_value(fd, t->prop_colorspace, "BT2020_RGB",
			     &t->colorspace_bt2020_rgb)) {
		fprintf(stderr,
			"FAIL: Colorspace does not offer BT2020_RGB\n");
		return false;
	}
	if (!find_enum_value(fd, t->prop_colorspace, "Default",
			     &t->colorspace_default)) {
		fprintf(stderr, "FAIL: Colorspace does not offer Default\n");
		return false;
	}
	return true;
}

/* One dumb framebuffer per flip, so each commit really delivers a new frame. */
static uint32_t make_fb(int fd)
{
	struct drm_mode_create_dumb create = {
		.width = W, .height = H, .bpp = 32,
	};
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint32_t fb = 0;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("FAIL CREATE_DUMB");
		return 0;
	}
	handles[0] = create.handle;
	pitches[0] = create.pitch;
	if (drmModeAddFB2(fd, W, H, DRM_FORMAT_XRGB2101010, handles, pitches,
			  offsets, &fb, 0)) {
		perror("FAIL drmModeAddFB2");
		return 0;
	}
	return fb;
}

/*
 * @colorspace and @hdr_blob are committed on the connector; @hdr_blob of 0 means
 * "no metadata", which is how a compositor leaves HDR. Returns the ioctl's own
 * errno on failure so a caller testing a rejection can tell EINVAL from the rest.
 */
static int commit(int fd, const struct kms_target *t, uint32_t fb,
		  uint32_t mode_blob, uint64_t colorspace, uint32_t hdr_blob,
		  bool allow_modeset)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	uint32_t flags = allow_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
	int ret;

	if (!req)
		return ENOMEM;

	drmModeAtomicAddProperty(req, t->crtc_id, t->prop_crtc_active, 1);
	drmModeAtomicAddProperty(req, t->crtc_id, t->prop_crtc_mode_id,
				 mode_blob);
	drmModeAtomicAddProperty(req, t->connector_id, t->prop_crtc_id,
				 t->crtc_id);
	drmModeAtomicAddProperty(req, t->connector_id, t->prop_colorspace,
				 colorspace);
	drmModeAtomicAddProperty(req, t->connector_id, t->prop_hdr_metadata,
				 hdr_blob);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_fb_id, fb);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_crtc_id,
				 t->crtc_id);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_src_x, 0);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_src_y, 0);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_src_w,
				 (uint64_t)W << 16);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_src_h,
				 (uint64_t)H << 16);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_crtc_x, 0);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_crtc_y, 0);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_crtc_w, W);
	drmModeAtomicAddProperty(req, t->plane_id, t->prop_plane_crtc_h, H);

	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret)
		ret = errno ? errno : EIO;
	drmModeAtomicFree(req);
	return ret;
}

/* A connector-only commit changes the output's colour without a new buffer. */
static int commit_color_only(int fd, const struct kms_target *t,
			     uint64_t colorspace, uint32_t hdr_blob)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret;

	if (!req)
		return ENOMEM;
	drmModeAtomicAddProperty(req, t->connector_id, t->prop_colorspace,
				 colorspace);
	drmModeAtomicAddProperty(req, t->connector_id, t->prop_hdr_metadata,
				 hdr_blob);
	ret = drmModeAtomicCommit(fd, req, 0, NULL);
	if (ret)
		ret = errno ? errno : EIO;
	drmModeAtomicFree(req);
	return ret;
}

static int acquire(int render_fd, struct drm_hermes_kms_acquire_frame2 *frame)
{
	memset(frame, 0, sizeof(*frame));
	if (ioctl(render_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, frame) < 0)
		return -1;
	return 0;
}

static bool check_hdr_fields(const struct drm_hermes_kms_acquire_frame2 *f)
{
	const struct hdr_metadata_infoframe *hdr =
		&f->color.hdr.hdmi_metadata_type1;
	bool ok = true;

#define CHECK(field, expected)                                              \
	do {                                                                \
		if ((unsigned)(hdr->field) != (unsigned)(expected)) {        \
			fprintf(stderr, "FAIL: %s is %u, expected %u\n",     \
				#field, (unsigned)(hdr->field),             \
				(unsigned)(expected));                      \
			ok = false;                                         \
		}                                                           \
	} while (0)

	CHECK(eotf, expected_hdr.eotf);
	CHECK(metadata_type, expected_hdr.metadata_type);
	for (unsigned int i = 0; i < 3; i++) {
		if (hdr->display_primaries[i].x !=
		    expected_hdr.display_primaries[i].x ||
		    hdr->display_primaries[i].y !=
		    expected_hdr.display_primaries[i].y) {
			fprintf(stderr,
				"FAIL: primary %u is (%u,%u), expected (%u,%u)\n",
				i, hdr->display_primaries[i].x,
				hdr->display_primaries[i].y,
				expected_hdr.display_primaries[i].x,
				expected_hdr.display_primaries[i].y);
			ok = false;
		}
	}
	CHECK(white_point.x, expected_hdr.white_point.x);
	CHECK(white_point.y, expected_hdr.white_point.y);
	CHECK(max_display_mastering_luminance,
	      expected_hdr.max_display_mastering_luminance);
	CHECK(min_display_mastering_luminance,
	      expected_hdr.min_display_mastering_luminance);
	CHECK(max_cll, expected_hdr.max_cll);
	CHECK(max_fall, expected_hdr.max_fall);
#undef CHECK
	return ok;
}

int main(void)
{
	struct hermes_session_credentials credentials;
	struct drm_hermes_kms_acquire_frame2 frame;
	struct drm_hermes_kms_acquire_frame legacy;
	struct hdr_output_metadata metadata;
	struct kms_target target;
	struct drm_hermes_kms_set_output output = {
		.enabled = 1, .width = W, .height = H, .refresh_hz = 60,
	};
	uint32_t mode_blob = 0, hdr_blob = 0, bad_blob = 0;
	uint32_t bad_inner_blob = 0, fb = 0;
	uint64_t hdr_sequence;
	int card, render_fd, ret;

	memset(&target, 0, sizeof target);
	card = open_hermes_card();
	if (card < 0) {
		fprintf(stderr, "FAIL: no hermes-kms card\n");
		return 1;
	}
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0) {
		perror("FAIL SET_OUTPUT");
		return 1;
	}
	if (drmSetMaster(card)) {
		fprintf(stderr, "FAIL: drmSetMaster: %s\n", strerror(errno));
		return 1;
	}
	if (drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(card, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "FAIL: atomic client caps unavailable\n");
		return 1;
	}
	if (!discover(card, &target))
		return 1;
	printf("      connector %u, crtc %u, plane %u, mode %s\n",
	       target.connector_id, target.crtc_id, target.plane_id,
	       target.mode.name);

	if (drmModeCreatePropertyBlob(card, &target.mode, sizeof target.mode,
				      &mode_blob)) {
		perror("FAIL mode blob");
		return 1;
	}
	memset(&metadata, 0, sizeof metadata);
	metadata.metadata_type = 0;	/* Static Metadata Descriptor Type 1 */
	metadata.hdmi_metadata_type1 = expected_hdr;
	if (drmModeCreatePropertyBlob(card, &metadata, sizeof metadata,
				      &hdr_blob)) {
		perror("FAIL HDR metadata blob");
		return 1;
	}

	fb = make_fb(card);
	if (!fb)
		return 1;
	ret = commit(card, &target, fb, mode_blob,
		     target.colorspace_bt2020_rgb, hdr_blob, true);
	if (ret) {
		fprintf(stderr,
			"FAIL: commit with BT2020_RGB and HDR metadata: %s\n",
			strerror(ret));
		return 1;
	}
	printf("PASS: committed Colorspace=BT2020_RGB with HDR static metadata\n");

	memset(&credentials, 0, sizeof credentials);
	if (hermes_session_get_owner_token(card, &credentials) < 0) {
		perror("FAIL session capability handoff");
		hermes_session_forget(&credentials);
		return 1;
	}
	render_fd = hermes_session_open_bound_render_credentials(&credentials,
								 NULL, 0, NULL);
	hermes_session_forget(&credentials);
	if (render_fd < 0) {
		perror("FAIL: no matching bound hermes-kms render node");
		return 1;
	}

	if (acquire(render_fd, &frame) < 0) {
		perror("FAIL ACQUIRE_FRAME2 with HDR committed");
		return 1;
	}
	if (!(frame.color.flags & HERMES_KMS_COLOR_VALID)) {
		fprintf(stderr, "FAIL: COLOR_VALID not set after committing a colorspace\n");
		return 1;
	}
	if (!(frame.color.flags & HERMES_KMS_COLOR_HDR_VALID)) {
		fprintf(stderr, "FAIL: HDR_METADATA_VALID not set after committing metadata\n");
		return 1;
	}
	if (!(frame.color.flags & HERMES_KMS_COLOR_RGB_FULL_RANGE)) {
		fprintf(stderr, "FAIL: captured RGB was not marked full range\n");
		return 1;
	}
	if (frame.color.colorspace != HERMES_KMS_COLORSPACE_BT2020_RGB) {
		fprintf(stderr,
			"FAIL: colorspace is %u, expected %u (BT2020_RGB)\n",
			frame.color.colorspace, HERMES_KMS_COLORSPACE_BT2020_RGB);
		return 1;
	}
	if (!check_hdr_fields(&frame))
		return 1;
	if (frame.frame.format != DRM_FORMAT_XRGB2101010) {
		fprintf(stderr, "FAIL: frame format is 0x%08x, expected XRGB2101010\n",
			frame.frame.format);
		return 1;
	}
	hdr_sequence = frame.frame.sequence;
	printf("PASS: ten-bit ACQUIRE_FRAME2 returned BT2020_RGB and every mastering value intact\n");

	/* The old command remains usable and cannot expose the new colour payload. */
	memset(&legacy, 0, sizeof legacy);
	if (ioctl(render_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &legacy) < 0) {
		perror("FAIL: legacy ACQUIRE_FRAME was rejected");
		return 1;
	}
	if (legacy.framebuffer_id != frame.frame.framebuffer_id ||
	    memcmp(legacy.reserved,
		   (unsigned char[sizeof legacy.reserved]){ 0 },
		   sizeof legacy.reserved)) {
		fprintf(stderr, "FAIL: legacy ACQUIRE_FRAME changed ABI behaviour\n");
		return 1;
	}
	memset(&frame, 0, sizeof frame);
	frame.color.reserved[0] = 1;
	if (ioctl(render_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME2, &frame) == 0 ||
	    errno != EINVAL) {
		fprintf(stderr, "FAIL: output-only color fields were accepted on input\n");
		return 1;
	}
	printf("PASS: legacy acquisition works and reserved colour input is rejected\n");

	/* A colour-only commit is a capture update even when the pixels are reused. */
	ret = commit_color_only(card, &target, target.colorspace_default, 0);
	if (ret) {
		fprintf(stderr, "FAIL: colour-only commit: %s\n", strerror(ret));
		return 1;
	}
	if (acquire(render_fd, &frame) < 0) {
		perror("FAIL ACQUIRE_FRAME2 after colour-only commit");
		return 1;
	}
	if (frame.frame.sequence <= hdr_sequence ||
	    (frame.frame.flags & HERMES_KMS_FRAME_DAMAGE_VALID) ||
	    (frame.color.flags & HERMES_KMS_COLOR_HDR_VALID) ||
	    frame.color.colorspace != HERMES_KMS_COLORSPACE_DEFAULT) {
		fprintf(stderr,
			"FAIL: colour-only update was not published as full-frame SDR\n");
		return 1;
	}
	printf("PASS: colour-only commit advanced the sequence and cleared HDR immediately\n");

	/* A following SDR frame must retain the cleared colour state. */
	fb = make_fb(card);
	if (!fb)
		return 1;
	ret = commit(card, &target, fb, mode_blob, target.colorspace_default, 0,
		     false);
	if (ret) {
		fprintf(stderr, "FAIL: commit leaving HDR: %s\n",
			strerror(ret));
		return 1;
	}
	if (acquire(render_fd, &frame) < 0) {
		perror("FAIL ACQUIRE_FRAME2 after leaving HDR");
		return 1;
	}
	if (frame.color.flags & HERMES_KMS_COLOR_HDR_VALID) {
		fprintf(stderr,
			"FAIL: HDR_METADATA_VALID still set after the blob was dropped\n");
		return 1;
	}
	if (!(frame.color.flags & HERMES_KMS_COLOR_VALID) ||
	    frame.color.colorspace != HERMES_KMS_COLORSPACE_DEFAULT) {
		fprintf(stderr,
			"FAIL: expected COLOR_VALID with Default, got flags 0x%x colorspace %u\n",
			frame.color.flags, frame.color.colorspace);
		return 1;
	}
	if (frame.color.hdr.hdmi_metadata_type1.eotf ||
	    frame.color.hdr.hdmi_metadata_type1.max_cll ||
	    frame.color.hdr.hdmi_metadata_type1.max_fall) {
		fprintf(stderr,
			"FAIL: stale HDR fields after leaving HDR: eotf=%u max_cll=%u max_fall=%u\n",
			frame.color.hdr.hdmi_metadata_type1.eotf,
			frame.color.hdr.hdmi_metadata_type1.max_cll,
			frame.color.hdr.hdmi_metadata_type1.max_fall);
		return 1;
	}
	printf("PASS: dropping the blob cleared HDR_METADATA_VALID and the metadata fields\n");

	/*
	 * A descriptor id the driver cannot pass on must fail the commit. If
	 * it were accepted and dropped, the compositor would believe the output
	 * is in HDR while the consumer receives no metadata at all.
	 */
	memset(&metadata, 0, sizeof metadata);
	metadata.metadata_type = 2;
	metadata.hdmi_metadata_type1 = expected_hdr;
	if (drmModeCreatePropertyBlob(card, &metadata, sizeof metadata,
				      &bad_blob)) {
		perror("FAIL bad metadata blob");
		return 1;
	}
	fb = make_fb(card);
	if (!fb)
		return 1;
	ret = commit(card, &target, fb, mode_blob,
		     target.colorspace_bt2020_rgb, bad_blob, false);
	if (ret != EINVAL) {
		fprintf(stderr,
			"FAIL: commit with descriptor id 2 returned %s, expected EINVAL\n",
			ret ? strerror(ret) : "success");
		return 1;
	}
	printf("PASS: a non-Type-1 outer descriptor is refused at commit time\n");

	/* The nested InfoFrame has its own descriptor id and must agree. */
	metadata.metadata_type = 0;
	metadata.hdmi_metadata_type1.metadata_type = 2;
	if (drmModeCreatePropertyBlob(card, &metadata, sizeof metadata,
				      &bad_inner_blob)) {
		perror("FAIL bad inner metadata blob");
		return 1;
	}
	ret = commit(card, &target, fb, mode_blob,
		     target.colorspace_bt2020_rgb, bad_inner_blob, false);
	if (ret != EINVAL) {
		fprintf(stderr,
			"FAIL: commit with inner descriptor id 2 returned %s, expected EINVAL\n",
			ret ? strerror(ret) : "success");
		return 1;
	}
	printf("PASS: a non-Type-1 inner descriptor is refused at commit time\n");

	/* Disabling the output must not leave colour state behind. */
	output.enabled = 0;
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0) {
		perror("FAIL SET_OUTPUT disable");
		return 1;
	}
	if (acquire(render_fd, &frame) == 0 &&
	    (frame.color.flags & (HERMES_KMS_COLOR_VALID |
				  HERMES_KMS_COLOR_HDR_VALID))) {
		fprintf(stderr,
			"FAIL: colour flags survive a disabled output (flags 0x%x)\n",
			frame.color.flags);
		return 1;
	}
	printf("PASS: disabling the output cleared the latched colour state\n");

	printf("\nRESULT: PASS - committed colour state reaches the capture consumer\n");
	return 0;
}
