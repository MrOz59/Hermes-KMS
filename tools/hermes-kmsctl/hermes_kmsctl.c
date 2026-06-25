// SPDX-License-Identifier: GPL-2.0

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/hermes_kms_drm.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [--device /dev/dri/cardN] version\n"
		"  %s [--device /dev/dri/cardN] caps\n"
		"  %s [--device /dev/dri/cardN] status\n"
		"  %s [--device /dev/dri/cardN] enable [WIDTHxHEIGHT@HZ]\n"
		"  %s [--device /dev/dri/cardN] disable\n",
		argv0, argv0, argv0, argv0, argv0);
}

static int open_if_hermes(const char *path)
{
	struct drm_hermes_kms_version version;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	memset(&version, 0, sizeof(version));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) == 0 &&
	    strcmp(version.driver_name, "hermes-kms") == 0)
		return fd;

	close(fd);
	return -1;
}

static int open_auto_device(void)
{
	struct dirent *entry;
	DIR *dir;
	int fd = -1;

	dir = opendir("/dev/dri");
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];

		if (strncmp(entry->d_name, "card", 4) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
		fd = open_if_hermes(path);
		if (fd >= 0)
			break;
	}

	closedir(dir);
	return fd;
}

static int open_device(const char *path)
{
	if (path)
		return open_if_hermes(path);

	return open_auto_device();
}

static int print_version(int fd)
{
	struct drm_hermes_kms_version version;

	memset(&version, 0, sizeof(version));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) < 0) {
		perror("GET_VERSION");
		return 1;
	}

	printf("driver=%s\n", version.driver_name);
	printf("uapi_version=%u\n", version.uapi_version);
	printf("driver_version=%u.%u.%u\n",
	       version.driver_major,
	       version.driver_minor,
	       version.driver_patch);

	return 0;
}

static int print_caps(int fd)
{
	struct drm_hermes_kms_caps caps;

	memset(&caps, 0, sizeof(caps));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_CAPS, &caps) < 0) {
		perror("GET_CAPS");
		return 1;
	}

	printf("flags=0x%016llx\n", (unsigned long long)caps.flags);
	printf("min=%ux%u\n", caps.min_width, caps.min_height);
	printf("max=%ux%u\n", caps.max_width, caps.max_height);
	printf("preferred=%ux%u\n", caps.preferred_width, caps.preferred_height);
	printf("max_refresh_hz=%u\n", caps.max_refresh_hz);
	printf("virtual_output=%s\n",
	       (caps.flags & HERMES_KMS_CAP_VIRTUAL_OUTPUT) ? "true" : "false");
	printf("output_control=%s\n",
	       (caps.flags & HERMES_KMS_CAP_OUTPUT_CONTROL) ? "true" : "false");
	printf("zero_copy_target=%s\n",
	       (caps.flags & HERMES_KMS_CAP_ZERO_COPY_TARGET) ? "true" : "false");
	printf("dmabuf_export_planned=%s\n",
	       (caps.flags & HERMES_KMS_CAP_DMABUF_EXPORT_PLANNED) ? "true" : "false");

	return 0;
}

static int print_status(int fd)
{
	struct drm_hermes_kms_status status;

	memset(&status, 0, sizeof(status));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) < 0) {
		perror("GET_STATUS");
		return 1;
	}

	printf("flags=0x%016llx\n", (unsigned long long)status.flags);
	printf("enabled=%s\n",
	       (status.flags & HERMES_KMS_STATUS_OUTPUT_ENABLED) ? "true" : "false");
	printf("connected=%s\n",
	       (status.flags & HERMES_KMS_STATUS_CONNECTED) ? "true" : "false");
	printf("scanout_active=%s\n",
	       (status.flags & HERMES_KMS_STATUS_SCANOUT_ACTIVE) ? "true" : "false");
	printf("frame_valid=%s\n",
	       (status.flags & HERMES_KMS_STATUS_FRAME_VALID) ? "true" : "false");
	printf("dmabuf_export_ready=%s\n",
	       (status.flags & HERMES_KMS_STATUS_DMABUF_EXPORT_READY) ? "true" : "false");
	printf("connector_id=%u\n", status.connector_id);
	printf("crtc_id=%u\n", status.crtc_id);
	printf("plane_id=%u\n", status.plane_id);
	printf("encoder_id=%u\n", status.encoder_id);
	printf("requested=%ux%u@%u\n",
	       status.requested_width,
	       status.requested_height,
	       status.requested_refresh_hz);
	printf("active=%ux%u@%u\n",
	       status.active_width,
	       status.active_height,
	       status.active_refresh_hz);
	printf("frame_sequence=%llu\n",
	       (unsigned long long)status.frame_sequence);
	printf("last_update_ns=%llu\n",
	       (unsigned long long)status.last_update_ns);
	printf("last_enable_ns=%llu\n",
	       (unsigned long long)status.last_enable_ns);
	printf("last_disable_ns=%llu\n",
	       (unsigned long long)status.last_disable_ns);
	printf("framebuffer_id=%u\n", status.framebuffer_id);
	printf("framebuffer_size=%ux%u\n",
	       status.framebuffer_width,
	       status.framebuffer_height);
	printf("framebuffer_format=0x%08x\n", status.framebuffer_format);
	printf("framebuffer_modifier=0x%016llx\n",
	       (unsigned long long)status.framebuffer_modifier);
	printf("framebuffer_plane_count=%u\n", status.framebuffer_plane_count);
	for (uint32_t i = 0; i < status.framebuffer_plane_count && i < 4; i++) {
		printf("framebuffer_plane_%u_pitch=%u\n",
		       i, status.framebuffer_pitch[i]);
		printf("framebuffer_plane_%u_offset=%u\n",
		       i, status.framebuffer_offset[i]);
	}

	return 0;
}

static bool parse_mode(const char *value, uint32_t *width, uint32_t *height,
		       uint32_t *refresh_hz)
{
	unsigned int parsed_width;
	unsigned int parsed_height;
	unsigned int parsed_refresh;

	if (sscanf(value, "%ux%u@%u", &parsed_width, &parsed_height,
		   &parsed_refresh) != 3)
		return false;

	*width = parsed_width;
	*height = parsed_height;
	*refresh_hz = parsed_refresh;
	return true;
}

static int set_output(int fd, bool enabled, const char *mode)
{
	struct drm_hermes_kms_set_output request;

	memset(&request, 0, sizeof(request));
	request.enabled = enabled ? 1 : 0;

	if (enabled && mode &&
	    !parse_mode(mode, &request.width, &request.height,
			&request.refresh_hz)) {
		fprintf(stderr, "Invalid mode '%s', expected WIDTHxHEIGHT@HZ\n",
			mode);
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &request) < 0) {
		perror("SET_OUTPUT");
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	const char *command;
	int argi = 1;
	int fd;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (argc >= 4 && strcmp(argv[argi], "--device") == 0) {
		device = argv[argi + 1];
		argi += 2;
	}

	if (argi >= argc) {
		usage(argv[0]);
		return 2;
	}

	command = argv[argi++];
	fd = open_device(device);
	if (fd < 0) {
		fprintf(stderr, "Could not find/open Hermes-KMS DRM device");
		if (device)
			fprintf(stderr, " at %s", device);
		fprintf(stderr, "\n");
		return 1;
	}

	if (strcmp(command, "version") == 0)
		ret = print_version(fd);
	else if (strcmp(command, "caps") == 0)
		ret = print_caps(fd);
	else if (strcmp(command, "status") == 0)
		ret = print_status(fd);
	else if (strcmp(command, "enable") == 0)
		ret = set_output(fd, true, argi < argc ? argv[argi] : NULL);
	else if (strcmp(command, "disable") == 0)
		ret = set_output(fd, false, NULL);
	else {
		usage(argv[0]);
		ret = 2;
	}

	close(fd);
	return ret;
}
