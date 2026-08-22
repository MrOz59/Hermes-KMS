// SPDX-License-Identifier: GPL-2.0
// hermes-vblank-meter: measure the Hermes-KMS software vblank rate directly.
//
// The tool claims the already advertised output mode through the generic
// UAPI, becomes DRM master, installs a dumb framebuffer, then measures N
// relative DRM vblank waits. Claiming the exact connector mode preserves the
// initial width, height and refresh selected by the caller/module parameters.
//
// Build: cc -O2 -I/usr/include/libdrm
//        -o hermes-vblank-meter hermes-vblank-meter.c -ldrm
// Run:   hermes-vblank-meter [vblanks]   (default 120)

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../include/uapi/drm/hermes_kms_drm.h"

static double now_s(void)
{
	struct timespec timestamp;

	if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0)
		return -1.0;
	return (double)timestamp.tv_sec + (double)timestamp.tv_nsec / 1.0e9;
}

static bool parse_card_index(const char *name, unsigned int *index)
{
	const char *suffix;
	char *end = NULL;
	unsigned long value;

	if (strncmp(name, "card", 4) != 0)
		return false;
	suffix = name + 4;
	if (!*suffix)
		return false;
	for (const char *character = suffix; *character; character++) {
		if (!isdigit((unsigned char)*character))
			return false;
	}
	errno = 0;
	value = strtoul(suffix, &end, 10);
	if (errno || !end || *end || value > UINT_MAX)
		return false;
	*index = (unsigned int)value;
	return true;
}

static bool is_hermes(int fd)
{
	drmVersionPtr version = drmGetVersion(fd);
	bool matches = version && version->name &&
		strcmp(version->name, "hermes-kms") == 0;

	if (version)
		drmFreeVersion(version);
	return matches;
}

static int open_hermes_card(void)
{
	DIR *directory = opendir("/dev/dri");
	struct dirent *entry;
	char best_path[PATH_MAX] = {0};
	unsigned int best_index = UINT_MAX;

	while (directory && (entry = readdir(directory))) {
		unsigned int index;
		char path[PATH_MAX];
		int length;
		int candidate;

		if (!parse_card_index(entry->d_name, &index) || index >= best_index)
			continue;
		length = snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;
		candidate = open(path, O_RDWR | O_CLOEXEC);
		if (candidate < 0)
			continue;
		if (is_hermes(candidate)) {
			memcpy(best_path, path, strlen(path) + 1U);
			best_index = index;
		}
		close(candidate);
	}
	if (directory)
		closedir(directory);
	if (!best_path[0]) {
		errno = ENODEV;
		return -1;
	}
	return open(best_path, O_RDWR | O_CLOEXEC);
}

static int parse_target(int argc, char **argv, unsigned int *target)
{
	char *end = NULL;
	unsigned long parsed;

	if (argc > 2)
		return -1;
	if (argc == 1) {
		*target = 120U;
		return 0;
	}
	errno = 0;
	parsed = strtoul(argv[1], &end, 10);
	if (errno || !end || *end || parsed < 1U || parsed > 1000000U)
		return -1;
	*target = (unsigned int)parsed;
	return 0;
}

static uint32_t connector_refresh(const drmModeModeInfo *mode)
{
	/* Hermes advertises the integer refresh requested through SET_OUTPUT. */
	return mode->vrefresh;
}

static uint32_t connector_crtc(int fd, const drmModeRes *resources,
			       const drmModeConnector *connector)
{
	for (int encoder_index = -1; encoder_index < connector->count_encoders;
	     encoder_index++) {
		uint32_t encoder_id = encoder_index < 0 ? connector->encoder_id :
			connector->encoders[encoder_index];
		drmModeEncoder *encoder;

		if (!encoder_id)
			continue;
		encoder = drmModeGetEncoder(fd, encoder_id);
		if (!encoder)
			continue;
		if (encoder->crtc_id) {
			uint32_t current = encoder->crtc_id;

			drmModeFreeEncoder(encoder);
			return current;
		}
		for (int crtc_index = 0;
		     crtc_index < resources->count_crtcs && crtc_index < 32;
		     crtc_index++) {
			if (encoder->possible_crtcs & (1U << (unsigned int)crtc_index)) {
				uint32_t possible = resources->crtcs[crtc_index];

				drmModeFreeEncoder(encoder);
				return possible;
			}
		}
		drmModeFreeEncoder(encoder);
	}
	return resources->count_crtcs == 1 ? resources->crtcs[0] : 0;
}

static int crtc_index(const drmModeRes *resources, uint32_t crtc_id)
{
	for (int index = 0; index < resources->count_crtcs; index++) {
		if (resources->crtcs[index] == crtc_id)
			return index;
	}
	return -1;
}

static bool make_vblank_type(int index, drmVBlankSeqType *type)
{
	const unsigned int max_index =
		DRM_VBLANK_HIGH_CRTC_MASK >> DRM_VBLANK_HIGH_CRTC_SHIFT;

	if (index < 0 || (unsigned int)index > max_index)
		return false;
	*type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE |
		((unsigned int)index << DRM_VBLANK_HIGH_CRTC_SHIFT));
	return true;
}

int main(int argc, char **argv)
{
	unsigned int target;
	int fd = -1;
	drmModeRes *resources = NULL;
	drmModeConnector *connector = NULL;
	drmModeModeInfo mode;
	uint32_t refresh = 0;
	uint32_t crtc_id = 0;
	drmVBlankSeqType vblank_type = DRM_VBLANK_RELATIVE;
	uint32_t fb_id = 0;
	uint32_t dumb_handle = 0;
	bool master = false;
	bool owns_output = false;
	int result = 2;

	if (parse_target(argc, argv, &target) < 0) {
		fprintf(stderr, "Usage: %s [vblanks] (1..1000000)\n", argv[0]);
		return 2;
	}

	fd = open_hermes_card();
	if (fd < 0) {
		fprintf(stderr, "hermes-kms card not found\n");
		goto out;
	}
	struct drm_hermes_kms_version version;
	struct drm_hermes_kms_caps capabilities;
	const uint64_t required_capabilities = HERMES_KMS_CAP_OUTPUT_CONTROL |
		HERMES_KMS_CAP_SESSION_OWNER | HERMES_KMS_CAP_SESSION_TOKEN;

	memset(&version, 0, sizeof(version));
	memset(&capabilities, 0, sizeof(capabilities));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) < 0 ||
	    ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_CAPS, &capabilities) < 0) {
		perror("Hermes UAPI discovery");
		goto out;
	}
	if (version.uapi_version < HERMES_KMS_UAPI_VERSION ||
	    (capabilities.flags & required_capabilities) != required_capabilities) {
		fprintf(stderr, "secure Hermes-KMS UAPI v%u session ownership is required\n",
			HERMES_KMS_UAPI_VERSION);
		goto out;
	}
	/* Establish exclusive KMS control before mutating the output session. */
	if (drmSetMaster(fd) != 0) {
		perror("drmSetMaster");
		goto out;
	}
	master = true;

	resources = drmModeGetResources(fd);
	if (!resources) {
		fprintf(stderr, "no DRM resources\n");
		goto out;
	}
	if (resources->count_crtcs < 1) {
		fprintf(stderr, "no usable CRTC\n");
		goto out;
	}

	for (int i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *candidate =
			drmModeGetConnector(fd, resources->connectors[i]);

		if (candidate && candidate->connection == DRM_MODE_CONNECTED &&
		    candidate->count_modes > 0) {
			connector = candidate;
			break;
		}
		if (candidate)
			drmModeFreeConnector(candidate);
	}
	if (!connector) {
		fprintf(stderr, "no connected connector\n");
		goto out;
	}

	int preferred_mode = 0;
	for (int i = 0; i < connector->count_modes; i++) {
		if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			preferred_mode = i;
			break;
		}
	}
	mode = connector->modes[preferred_mode];
	refresh = connector_refresh(&mode);
	if (!mode.hdisplay || !mode.vdisplay || !refresh) {
		fprintf(stderr, "connected connector has no valid preferred mode\n");
		goto out;
	}
	crtc_id = connector_crtc(fd, resources, connector);
	if (!crtc_id) {
		fprintf(stderr, "connected connector has no compatible CRTC\n");
		goto out;
	}
	int selected_crtc = crtc_index(resources, crtc_id);
	if (!make_vblank_type(selected_crtc, &vblank_type)) {
		fprintf(stderr, "CRTC cannot be encoded in DRM_WAIT_VBLANK\n");
		goto out;
	}
	if (capabilities.output_count > 1U) {
		struct drm_hermes_kms_select_output select;

		if (selected_crtc < 0 ||
		    (uint32_t)selected_crtc >= capabilities.output_count) {
			fprintf(stderr, "cannot map connector CRTC to Hermes output\n");
			goto out;
		}
		memset(&select, 0, sizeof(select));
		select.output_index = (uint32_t)selected_crtc;
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT, &select) < 0) {
			perror("SELECT_OUTPUT");
			goto out;
		}
		if (select.selected_output_index != (uint32_t)selected_crtc) {
			fprintf(stderr, "SELECT_OUTPUT selected the wrong output\n");
			goto out;
		}
	}

	/*
	 * This generic ownership operation is intentionally application-neutral.
	 * Reusing the connector's mode claims the initial session without changing
	 * the refresh rate that this diagnostic is meant to measure.
	 */
	struct drm_hermes_kms_set_output output = {
		.enabled = 1,
		.width = mode.hdisplay,
		.height = mode.vdisplay,
		.refresh_hz = refresh,
	};
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) < 0) {
		perror("SET_OUTPUT (claim session)");
		goto out;
	}
	owns_output = true;
	if (!(output.result_flags & HERMES_KMS_SET_OUTPUT_RESULT_CONNECTED) ||
	    !(output.result_flags & HERMES_KMS_SET_OUTPUT_RESULT_OWNER_ASSIGNED) ||
	    !output.session_id || output.width != (uint32_t)mode.hdisplay ||
	    output.height != (uint32_t)mode.vdisplay ||
	    output.refresh_hz != refresh) {
		fprintf(stderr, "SET_OUTPUT returned an inconsistent session/mode\n");
		goto out;
	}

	struct drm_mode_create_dumb create = {
		.width = mode.hdisplay,
		.height = mode.vdisplay,
		.bpp = 32,
	};
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		perror("CREATE_DUMB");
		goto out;
	}
	dumb_handle = create.handle;
	if (drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32,
			 create.pitch, create.handle, &fb_id) != 0) {
		perror("AddFB");
		goto out;
	}
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0,
			   &connector->connector_id, 1, &mode) != 0) {
		perror("SetCrtc");
		goto out;
	}

	printf("mode %ux%u@%u on connector %u crtc %u; waiting %u vblanks\n",
	       (unsigned int)mode.hdisplay, (unsigned int)mode.vdisplay, refresh,
	       connector->connector_id, crtc_id, target);

	drmVBlank vblank;
	memset(&vblank, 0, sizeof(vblank));
	vblank.request.type = vblank_type;
	vblank.request.sequence = 1;
	if (drmWaitVBlank(fd, &vblank) != 0) {
		perror("WAIT_VBLANK prime");
		goto out;
	}

	double started = now_s();
	if (started < 0.0) {
		perror("clock_gettime");
		goto out;
	}
	uint64_t missed = 0;
	uint32_t last = vblank.reply.sequence;
	for (unsigned int i = 0; i < target; i++) {
		drmVBlank wait;
		uint32_t delta;

		memset(&wait, 0, sizeof(wait));
		wait.request.type = vblank_type;
		wait.request.sequence = 1;
		if (drmWaitVBlank(fd, &wait) != 0) {
			perror("WAIT_VBLANK");
			goto out;
		}
		delta = wait.reply.sequence - last;
		if (delta > 1U)
			missed += (uint64_t)(delta - 1U);
		else if (delta == 0U)
			missed++;
		last = wait.reply.sequence;
	}
	double finished = now_s();
	if (finished < 0.0) {
		perror("clock_gettime");
		goto out;
	}
	double elapsed = finished - started;
	if (elapsed <= 0.0) {
		fprintf(stderr, "invalid measurement interval\n");
		goto out;
	}

	double measured = (double)target / elapsed;
	double deviation = (measured - (double)refresh) / (double)refresh * 100.0;
	if (deviation < 0.0)
		deviation = -deviation;
	printf("measured %.2f Hz over %.3fs (target %u, dev %.2f%%, missed %" PRIu64 ")\n",
	       measured, elapsed, refresh, deviation, missed);

	result = (deviation > 5.0 || missed != 0U) ? 1 : 0;

out:
	if (fb_id && drmModeRmFB(fd, fb_id) != 0) {
		perror("RmFB");
		if (result == 0)
			result = 1;
	}
	if (dumb_handle) {
		struct drm_mode_destroy_dumb destroy = {.handle = dumb_handle};

		if (drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) != 0) {
			perror("DESTROY_DUMB");
			if (result == 0)
				result = 1;
		}
	}
	if (connector)
		drmModeFreeConnector(connector);
	if (resources)
		drmModeFreeResources(resources);
	if (owns_output) {
		struct drm_hermes_kms_set_output disable;

		memset(&disable, 0, sizeof(disable));
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &disable) < 0) {
			perror("SET_OUTPUT (release session)");
			if (result == 0)
				result = 1;
		}
	}
	if (master && drmDropMaster(fd) != 0) {
		perror("drmDropMaster");
		if (result == 0)
			result = 1;
	}
	if (fd >= 0 && close(fd) != 0) {
		perror("close");
		if (result == 0)
			result = 1;
	}
	if (result <= 1)
		printf("%s\n", result ? "FAIL" : "PASS");
	return result;
}
