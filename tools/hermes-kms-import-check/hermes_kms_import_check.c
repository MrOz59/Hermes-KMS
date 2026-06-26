// SPDX-License-Identifier: GPL-2.0

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <drm/hermes_kms_drm.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

struct import_format {
	uint32_t drm_format;
	uint32_t va_fourcc;
	uint32_t va_rt_format;
	const char *name;
};

static const struct import_format import_formats[] = {
	{ DRM_FORMAT_XRGB8888, VA_FOURCC_XRGB, VA_RT_FORMAT_RGB32, "XRGB8888" },
	{ DRM_FORMAT_ARGB8888, VA_FOURCC_ARGB, VA_RT_FORMAT_RGB32, "ARGB8888" },
	{ DRM_FORMAT_XBGR8888, VA_FOURCC_XBGR, VA_RT_FORMAT_RGB32, "XBGR8888" },
	{ DRM_FORMAT_ABGR8888, VA_FOURCC_ABGR, VA_RT_FORMAT_RGB32, "ABGR8888" },
	{ DRM_FORMAT_NV12, VA_FOURCC_NV12, VA_RT_FORMAT_YUV420, "NV12" },
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/dri/cardN] [--va-device /dev/dri/renderDN] [--wait-ms MS]\n",
		argv0);
}

static const struct import_format *find_import_format(uint32_t drm_format)
{
	for (size_t i = 0; i < sizeof(import_formats) / sizeof(import_formats[0]); i++) {
		if (import_formats[i].drm_format == drm_format)
			return &import_formats[i];
	}

	return NULL;
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

static int open_auto_hermes(void)
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

static int open_hermes(const char *path)
{
	if (path)
		return open_if_hermes(path);

	return open_auto_hermes();
}

static int open_va_device(const char *path)
{
	struct dirent *entry;
	DIR *dir;
	int fd = -1;

	if (path)
		return open(path, O_RDWR | O_CLOEXEC);

	dir = opendir("/dev/dri");
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char candidate[PATH_MAX];

		if (strncmp(entry->d_name, "renderD", 7) != 0)
			continue;

		snprintf(candidate, sizeof(candidate), "/dev/dri/%s", entry->d_name);
		fd = open(candidate, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			break;
	}

	closedir(dir);
	return fd;
}

static int wait_for_frame(int fd, uint32_t wait_ms)
{
	struct drm_hermes_kms_status status;
	struct drm_hermes_kms_wait_frame wait;

	memset(&status, 0, sizeof(status));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) < 0) {
		perror("GET_STATUS");
		return 1;
	}

	if (status.flags & HERMES_KMS_STATUS_FRAME_VALID)
		return 0;

	memset(&wait, 0, sizeof(wait));
	wait.after_sequence = status.frame_sequence;
	wait.timeout_ms = wait_ms;

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) < 0) {
		perror("WAIT_FRAME");
		return 1;
	}

	if (!(wait.flags & HERMES_KMS_WAIT_FRAME_READY)) {
		fprintf(stderr, "WAIT_FRAME returned without a ready frame\n");
		return 1;
	}

	return 0;
}

static int acquire_frame(int fd, struct drm_hermes_kms_acquire_frame *frame)
{
	memset(frame, 0, sizeof(*frame));
	frame->flags = HERMES_KMS_FRAME_REQUEST_DMABUF |
		       HERMES_KMS_FRAME_REQUEST_SYNC_FILE;

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, frame) < 0) {
		perror("ACQUIRE_FRAME");
		return 1;
	}

	if (!(frame->flags & HERMES_KMS_FRAME_DMABUF_VALID)) {
		fprintf(stderr, "ACQUIRE_FRAME did not return DMA-BUF fds\n");
		return 1;
	}

	if (!frame->plane_count || frame->plane_count > 4) {
		fprintf(stderr, "invalid plane_count=%u\n", frame->plane_count);
		return 1;
	}

	for (uint32_t i = 0; i < frame->plane_count; i++) {
		if (frame->dma_buf_fd[i] < 0) {
			fprintf(stderr, "plane %u has no DMA-BUF fd\n", i);
			return 1;
		}
	}

	return 0;
}

static void close_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	for (uint32_t i = 0; i < frame->plane_count && i < 4; i++) {
		if (frame->dma_buf_fd[i] >= 0) {
			close(frame->dma_buf_fd[i]);
			frame->dma_buf_fd[i] = -1;
		}
	}

	if (frame->sync_file_fd >= 0) {
		close(frame->sync_file_fd);
		frame->sync_file_fd = -1;
	}
}

static void init_invalid_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	for (uint32_t i = 0; i < 4; i++)
		frame->dma_buf_fd[i] = -1;
	frame->sync_file_fd = -1;
}

static int import_with_vaapi(int va_fd,
			     const struct drm_hermes_kms_acquire_frame *frame)
{
	const struct import_format *format;
	VADisplay display;
	VAStatus status;
	VASurfaceID surface = VA_INVALID_SURFACE;
	VADRMPRIMESurfaceDescriptor descriptor;
	VASurfaceAttrib attribs[3];
	int major = 0;
	int minor = 0;

	format = find_import_format(frame->format);
	if (!format) {
		fprintf(stderr, "unsupported DRM format 0x%08x for VA import check\n",
			frame->format);
		return 1;
	}

	display = vaGetDisplayDRM(va_fd);
	if (!display) {
		fprintf(stderr, "vaGetDisplayDRM failed\n");
		return 1;
	}

	status = vaInitialize(display, &major, &minor);
	if (status != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize failed: %s\n", vaErrorStr(status));
		return 1;
	}

	memset(&descriptor, 0, sizeof(descriptor));
	descriptor.fourcc = format->va_fourcc;
	descriptor.width = frame->width;
	descriptor.height = frame->height;
	descriptor.num_objects = frame->plane_count;
	descriptor.num_layers = 1;
	descriptor.layers[0].drm_format = frame->format;
	descriptor.layers[0].num_planes = frame->plane_count;

	for (uint32_t i = 0; i < frame->plane_count; i++) {
		descriptor.objects[i].fd = frame->dma_buf_fd[i];
		descriptor.objects[i].size = frame->offset[i] +
					     frame->pitch[i] * frame->height;
		descriptor.objects[i].drm_format_modifier = frame->modifier;
		descriptor.layers[0].object_index[i] = i;
		descriptor.layers[0].offset[i] = frame->offset[i];
		descriptor.layers[0].pitch[i] = frame->pitch[i];
	}

	memset(attribs, 0, sizeof(attribs));
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &descriptor;

	attribs[2].type = VASurfaceAttribUsageHint;
	attribs[2].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[2].value.type = VAGenericValueTypeInteger;
	attribs[2].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER |
				   VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ;

	status = vaCreateSurfaces(display,
				  format->va_rt_format,
				  frame->width,
				  frame->height,
				  &surface,
				  1,
				  attribs,
				  3);
	if (status != VA_STATUS_SUCCESS) {
		fprintf(stderr,
			"VAAPI import failed for %s %ux%u modifier=0x%016llx: %s\n",
			format->name,
			frame->width,
			frame->height,
			(unsigned long long)frame->modifier,
			vaErrorStr(status));
		vaTerminate(display);
		return 1;
	}

	printf("vaapi_import=true\n");
	printf("va_version=%d.%d\n", major, minor);
	printf("surface_id=%u\n", surface);
	printf("format=%s\n", format->name);
	printf("size=%ux%u\n", frame->width, frame->height);
	printf("modifier=0x%016llx\n", (unsigned long long)frame->modifier);
	printf("sequence=%llu\n", (unsigned long long)frame->sequence);
	printf("sync_file_valid=%s\n",
	       (frame->flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) ? "true" : "false");

	vaDestroySurfaces(display, &surface, 1);
	vaTerminate(display);
	return 0;
}

int main(int argc, char **argv)
{
	const char *hermes_device = NULL;
	const char *va_device = NULL;
	uint32_t wait_ms = 1000;
	struct drm_hermes_kms_acquire_frame frame;
	int hermes_fd;
	int va_fd;
	int ret;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			hermes_device = argv[++i];
		} else if (strcmp(argv[i], "--va-device") == 0 && i + 1 < argc) {
			va_device = argv[++i];
		} else if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
			char *end = NULL;
			unsigned long parsed = strtoul(argv[++i], &end, 0);

			if (!end || *end || parsed > UINT32_MAX) {
				fprintf(stderr, "invalid wait timeout\n");
				return 2;
			}
			wait_ms = (uint32_t)parsed;
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	hermes_fd = open_hermes(hermes_device);
	if (hermes_fd < 0) {
		fprintf(stderr, "could not open Hermes-KMS device\n");
		return 1;
	}

	va_fd = open_va_device(va_device);
	if (va_fd < 0) {
		perror("open VA DRM device");
		close(hermes_fd);
		return 1;
	}

	ret = wait_for_frame(hermes_fd, wait_ms);
	if (ret)
		goto out;

	init_invalid_frame_fds(&frame);
	ret = acquire_frame(hermes_fd, &frame);
	if (ret)
		goto out;

	ret = import_with_vaapi(va_fd, &frame);
	close_frame_fds(&frame);

out:
	close(va_fd);
	close(hermes_fd);
	return ret;
}
