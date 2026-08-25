// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/hermes_kms_drm.h>

#include "../hermes_session.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [OPTIONS] version\n"
		"  %s [OPTIONS] outputs\n"
		"  %s [OPTIONS] identity\n"
		"  %s [OPTIONS] caps\n"
		"  %s [OPTIONS] status\n"
		"  %s [OPTIONS] metrics\n"
		"  %s [OPTIONS] diagnose\n"
		"  %s [OPTIONS] wait [AFTER_SEQUENCE] [TIMEOUT_MS]\n"
		"  %s [OPTIONS] frame [--require-dmabuf] [--sync-file]\n"
		"  %s [OPTIONS] enable [WIDTHxHEIGHT@HZ]  (holds until Ctrl+C)\n"
		"  %s [OPTIONS] hold [WIDTHxHEIGHT@HZ]\n"
		"  %s [OPTIONS] disable\n"
		"Options: --device PATH --output N --session-file PATH --control PATH --verbose\n"
		"For hold/enable, --session-file is created mode 0600; other commands read and bind it.\n"
		"--control PATH creates a private FIFO that hold/enable read commands from:\n"
		"  rotate  retire the current token, leaving running consumers bound,\n"
		"          and republish --session-file with the new one\n"
		"  revoke  drop every bound descriptor at once and remove --session-file\n",
		argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
		argv0, argv0, argv0, argv0);
}

static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int signal)
{
	(void)signal;
	stop_requested = 1;
}

static int open_if_hermes(const char *path, bool verbose)
{
	struct drm_hermes_kms_version version;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (verbose)
			fprintf(stderr, "%s: open failed: %s\n", path, strerror(errno));
		return -1;
	}

	memset(&version, 0, sizeof(version));
	if (hermes_session_require_driver(fd) == 0 &&
	    ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) == 0 &&
	    strcmp(version.driver_name, "hermes-kms") == 0)
		return fd;

	if (verbose)
		fprintf(stderr, "%s: not Hermes-KMS\n", path);

	close(fd);
	return -1;
}

static int open_auto_device(bool verbose)
{
	struct dirent *entry;
	DIR *dir;
	int best_fd = -1;
	unsigned int best_role_rank = UINT_MAX;
	unsigned int best_device_index = UINT_MAX;
	unsigned int best_node_rank = UINT_MAX;
	char best_path[PATH_MAX] = "";

	dir = opendir("/dev/dri");
	if (!dir) {
		if (verbose)
			fprintf(stderr, "/dev/dri: open failed: %s\n", strerror(errno));
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		struct drm_hermes_kms_identity identity;
		const char *number;
		char *end = NULL;
		char path[PATH_MAX];
		unsigned int node_rank;
		unsigned int role_rank;
		unsigned int device_index;
		int fd;

		if (strncmp(entry->d_name, "renderD", 7) == 0) {
			number = entry->d_name + 7;
			node_rank = 0;
		} else if (strncmp(entry->d_name, "card", 4) == 0) {
			number = entry->d_name + 4;
			node_rank = 1;
		} else {
			continue;
		}
		if (!*number)
			continue;
		errno = 0;
		(void)strtoul(number, &end, 10);
		if (errno || !end || *end)
			continue;

		snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
		fd = open_if_hermes(path, verbose);
		if (fd < 0)
			continue;

		memset(&identity, 0, sizeof(identity));
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY, &identity) == 0) {
			device_index = identity.device_index;
			switch (identity.device_role) {
			case HERMES_KMS_DEVICE_ROLE_HOST:
				role_rank = 0;
				break;
			case HERMES_KMS_DEVICE_ROLE_GENERAL:
				role_rank = 1;
				break;
			default:
				role_rank = 2;
				break;
			}
		} else {
			/* Pre-identity UAPI: retain deterministic path ordering. */
			device_index = UINT_MAX;
			role_rank = 1;
		}

		if (best_fd < 0 || role_rank < best_role_rank ||
		    (role_rank == best_role_rank && device_index < best_device_index) ||
		    (role_rank == best_role_rank && device_index == best_device_index &&
		     node_rank < best_node_rank) ||
		    (role_rank == best_role_rank && device_index == best_device_index &&
		     node_rank == best_node_rank && strcmp(path, best_path) < 0)) {
			if (best_fd >= 0)
				close(best_fd);
			best_fd = fd;
			best_role_rank = role_rank;
			best_device_index = device_index;
			best_node_rank = node_rank;
			strncpy(best_path, path, sizeof(best_path) - 1);
			best_path[sizeof(best_path) - 1] = '\0';
		} else {
			close(fd);
		}
	}

	closedir(dir);
	if (verbose && best_fd >= 0)
		fprintf(stderr, "auto-selected %s\n", best_path);
	return best_fd;
}

static int open_device(const char *path, bool verbose)
{
	if (path)
		return open_if_hermes(path, verbose);

	return open_auto_device(verbose);
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
	printf("output_count=%u\n", caps.output_count ? caps.output_count : 1);
	printf("virtual_output=%s\n",
	       (caps.flags & HERMES_KMS_CAP_VIRTUAL_OUTPUT) ? "true" : "false");
	printf("output_control=%s\n",
	       (caps.flags & HERMES_KMS_CAP_OUTPUT_CONTROL) ? "true" : "false");
	printf("frame_metadata=%s\n",
	       (caps.flags & HERMES_KMS_CAP_FRAME_METADATA) ? "true" : "false");
	printf("frame_acquire=%s\n",
	       (caps.flags & HERMES_KMS_CAP_FRAME_ACQUIRE) ? "true" : "false");
	printf("dmabuf_export=%s\n",
	       (caps.flags & HERMES_KMS_CAP_DMABUF_EXPORT) ? "true" : "false");
	printf("output_identity=%s\n",
	       (caps.flags & HERMES_KMS_CAP_OUTPUT_IDENTITY) ? "true" : "false");
	printf("session_owner=%s\n",
	       (caps.flags & HERMES_KMS_CAP_SESSION_OWNER) ? "true" : "false");
	printf("frame_wait=%s\n",
	       (caps.flags & HERMES_KMS_CAP_FRAME_WAIT) ? "true" : "false");
	printf("metrics=%s\n",
	       (caps.flags & HERMES_KMS_CAP_METRICS) ? "true" : "false");
	printf("multi_output=%s\n",
	       (caps.flags & HERMES_KMS_CAP_MULTI_OUTPUT) ? "true" : "false");
	printf("multi_device=%s\n",
	       (caps.flags & HERMES_KMS_CAP_MULTI_DEVICE) ? "true" : "false");
	printf("session_device_pool=%s\n",
	       (caps.flags & HERMES_KMS_CAP_SESSION_DEVICE_POOL) ? "true" : "false");
	printf("session_token=%s\n",
	       (caps.flags & HERMES_KMS_CAP_SESSION_TOKEN) ? "true" : "false");
	printf("zero_copy_target=%s\n",
	       (caps.flags & HERMES_KMS_CAP_ZERO_COPY_TARGET) ? "true" : "false");
	printf("writeback_connector=%s\n",
	       (caps.flags & HERMES_KMS_CAP_WRITEBACK_CONNECTOR) ? "true" : "false");
	printf("sync_file=%s\n",
	       (caps.flags & HERMES_KMS_CAP_SYNC_FILE) ? "true" : "false");
	printf("dmabuf_export_planned=%s\n",
	       (caps.flags & HERMES_KMS_CAP_DMABUF_EXPORT_PLANNED) ? "true" : "false");

	return 0;
}

static int print_metrics(int fd)
{
	struct drm_hermes_kms_metrics metrics;

	memset(&metrics, 0, sizeof(metrics));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_METRICS, &metrics) < 0) {
		perror("GET_METRICS");
		return 1;
	}

	printf("frame_sequence=%llu\n", (unsigned long long)metrics.frame_sequence);
	printf("frame_update_count=%llu\n", (unsigned long long)metrics.frame_update_count);
	printf("acquire_count=%llu\n", (unsigned long long)metrics.acquire_count);
	printf("acquire_no_frame_count=%llu\n", (unsigned long long)metrics.acquire_no_frame_count);
	printf("dmabuf_export_count=%llu\n", (unsigned long long)metrics.dmabuf_export_count);
	printf("dmabuf_export_fail_count=%llu\n", (unsigned long long)metrics.dmabuf_export_fail_count);
	printf("sync_file_export_count=%llu\n", (unsigned long long)metrics.sync_file_export_count);
	printf("sync_file_export_fail_count=%llu\n", (unsigned long long)metrics.sync_file_export_fail_count);
	printf("wait_count=%llu\n", (unsigned long long)metrics.wait_count);
	printf("wait_ready_count=%llu\n", (unsigned long long)metrics.wait_ready_count);
	printf("wait_timeout_count=%llu\n", (unsigned long long)metrics.wait_timeout_count);
	printf("wait_interrupted_count=%llu\n", (unsigned long long)metrics.wait_interrupted_count);
	printf("output_enable_count=%llu\n", (unsigned long long)metrics.output_enable_count);
	printf("output_disable_count=%llu\n", (unsigned long long)metrics.output_disable_count);
	printf("hotplug_event_count=%llu\n", (unsigned long long)metrics.hotplug_event_count);
	printf("owner_close_disconnect_count=%llu\n", (unsigned long long)metrics.owner_close_disconnect_count);
	printf("last_update_ns=%llu\n", (unsigned long long)metrics.last_update_ns);
	printf("last_acquire_ns=%llu\n", (unsigned long long)metrics.last_acquire_ns);
	printf("last_wait_start_ns=%llu\n", (unsigned long long)metrics.last_wait_start_ns);
	printf("last_wait_end_ns=%llu\n", (unsigned long long)metrics.last_wait_end_ns);
	printf("last_wait_duration_ns=%llu\n", (unsigned long long)metrics.last_wait_duration_ns);
	printf("last_dmabuf_export_ns=%llu\n", (unsigned long long)metrics.last_dmabuf_export_ns);
	printf("last_sync_file_export_ns=%llu\n", (unsigned long long)metrics.last_sync_file_export_ns);
	printf("vblank_count=%llu\n", (unsigned long long)metrics.vblank_count);
	printf("vblank_overrun_count=%llu\n",
	       (unsigned long long)metrics.vblank_overrun_count);

	return 0;
}

static int parse_u64(const char *value, uint64_t *out)
{
	char *end;
	unsigned long long parsed;

	if (!value || !*value || *value == '-')
		return -1;
	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !end || *end)
		return -1;

	*out = parsed;
	return 0;
}

static int parse_u32(const char *value, uint32_t *out)
{
	uint64_t parsed;

	if (parse_u64(value, &parsed) < 0 || parsed > UINT32_MAX)
		return -1;

	*out = (uint32_t)parsed;
	return 0;
}

static int wait_frame(int fd, int argc, char **argv)
{
	struct drm_hermes_kms_wait_frame wait;
	uint64_t after_sequence = 0;

	memset(&wait, 0, sizeof(wait));
	wait.timeout_ms = 1000;

	if (argc > 0) {
		if (parse_u64(argv[0], &after_sequence) < 0) {
			fprintf(stderr, "Invalid AFTER_SEQUENCE '%s'\n", argv[0]);
			return 2;
		}
		wait.after_sequence = after_sequence;
	}

	if (argc > 1 && parse_u32(argv[1], &wait.timeout_ms) < 0) {
		fprintf(stderr, "Invalid TIMEOUT_MS '%s'\n", argv[1]);
		return 2;
	}

	if (argc > 2) {
		fprintf(stderr, "Too many arguments for wait\n");
		return 2;
	}

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) < 0) {
		perror("WAIT_FRAME");
		return 1;
	}

	printf("flags=0x%016llx\n", (unsigned long long)wait.flags);
	printf("frame_ready=%s\n",
	       (wait.flags & HERMES_KMS_WAIT_FRAME_READY) ? "true" : "false");
	printf("sequence=%llu\n", (unsigned long long)wait.sequence);
	printf("timestamp_ns=%llu\n", (unsigned long long)wait.timestamp_ns);
	printf("status_flags=0x%016llx\n",
	       (unsigned long long)wait.status_flags);
	printf("enabled=%s\n",
	       (wait.status_flags & HERMES_KMS_STATUS_OUTPUT_ENABLED) ? "true" : "false");
	printf("connected=%s\n",
	       (wait.status_flags & HERMES_KMS_STATUS_CONNECTED) ? "true" : "false");
	printf("frame_valid=%s\n",
	       (wait.status_flags & HERMES_KMS_STATUS_FRAME_VALID) ? "true" : "false");
	printf("dmabuf_export_ready=%s\n",
	       (wait.status_flags & HERMES_KMS_STATUS_DMABUF_EXPORT_READY) ? "true" : "false");
	printf("session_owned=%s\n",
	       (wait.status_flags & HERMES_KMS_STATUS_SESSION_OWNED) ? "true" : "false");

	return 0;
}

static int print_identity(int fd)
{
	struct drm_hermes_kms_identity identity;

	memset(&identity, 0, sizeof(identity));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY, &identity) < 0) {
		perror("GET_IDENTITY");
		return 1;
	}

	printf("driver=%s\n", identity.driver_name);
	printf("output=%s\n", identity.output_name);
	printf("connector=%s\n", identity.connector_name);
	printf("connector_id=%u\n", identity.connector_id);
	printf("crtc_id=%u\n", identity.crtc_id);
	printf("plane_id=%u\n", identity.plane_id);
	printf("encoder_id=%u\n", identity.encoder_id);
	printf("output_index=%u\n", identity.output_index + 1);
	printf("output_count=%u\n",
	       identity.output_count ? identity.output_count : 1);
	printf("device_index=%u\n", identity.device_index + 1);
	printf("device_count=%u\n",
	       identity.device_count ? identity.device_count : 1);
	printf("device_role=%s\n",
	       identity.device_role == HERMES_KMS_DEVICE_ROLE_HOST ? "host" :
	       identity.device_role == HERMES_KMS_DEVICE_ROLE_SESSION ? "session" :
	       "general");
	printf("session_index=%u\n", identity.session_index);
	printf("session_device_count=%u\n", identity.session_device_count);

	return 0;
}

static int select_output(int fd, uint32_t output_number)
{
	struct drm_hermes_kms_select_output request;

	if (!output_number) {
		fprintf(stderr, "Output numbers start at 1\n");
		return 2;
	}

	memset(&request, 0, sizeof(request));
	request.output_index = output_number - 1;
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT, &request) < 0) {
		fprintf(stderr, "SELECT_OUTPUT %u failed: %s\n",
			output_number, strerror(errno));
		return 1;
	}

	return 0;
}

static int print_outputs(int fd)
{
	struct drm_hermes_kms_caps caps;
	uint32_t count;

	memset(&caps, 0, sizeof(caps));
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_CAPS, &caps) < 0) {
		perror("GET_CAPS");
		return 1;
	}

	count = caps.output_count ? caps.output_count : 1;
	printf("output_count=%u\n", count);
	for (uint32_t i = 0; i < count; i++) {
		struct drm_hermes_kms_identity identity;
		struct drm_hermes_kms_status status;

		if (count > 1 && select_output(fd, i + 1))
			return 1;

		memset(&identity, 0, sizeof(identity));
		memset(&status, 0, sizeof(status));
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY, &identity) < 0) {
			perror("GET_IDENTITY");
			return 1;
		}
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) < 0) {
			perror("GET_STATUS");
			return 1;
		}

		printf("output_%u_name=%s\n", i + 1, identity.output_name);
		printf("output_%u_connector=%s\n", i + 1,
		       identity.connector_name);
		printf("output_%u_enabled=%s\n", i + 1,
		       (status.flags & HERMES_KMS_STATUS_OUTPUT_ENABLED) ?
		       "true" : "false");
		printf("output_%u_owner_pid=%d\n", i + 1, status.owner_pid);
		printf("output_%u_frame_sequence=%llu\n", i + 1,
		       (unsigned long long)status.frame_sequence);
	}

	return 0;
}

static int print_frame(int fd, int argc, char **argv)
{
	struct drm_hermes_kms_acquire_frame frame;
	bool require_dmabuf = false;
	bool require_sync_file = false;
	int ret = 0;

	memset(&frame, 0, sizeof(frame));
	for (uint32_t i = 0; i < 4; i++)
		frame.dma_buf_fd[i] = -1;
	frame.sync_file_fd = -1;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--require-dmabuf") == 0) {
			frame.flags |= HERMES_KMS_FRAME_REQUEST_DMABUF;
			require_dmabuf = true;
		} else if (strcmp(argv[i], "--sync-file") == 0) {
			frame.flags |= HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
			require_sync_file = true;
		} else {
			fprintf(stderr, "Unknown frame option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &frame) < 0) {
		perror("ACQUIRE_FRAME");
		return 1;
	}

	printf("flags=0x%016llx\n", (unsigned long long)frame.flags);
	printf("metadata_valid=%s\n",
	       (frame.flags & HERMES_KMS_FRAME_METADATA_VALID) ? "true" : "false");
	printf("dmabuf_valid=%s\n",
	       (frame.flags & HERMES_KMS_FRAME_DMABUF_VALID) ? "true" : "false");
	printf("sync_file_valid=%s\n",
	       (frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) ? "true" : "false");
	printf("copy_fallback_required=%s\n",
	       (frame.flags & HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED) ? "true" : "false");
	printf("sequence=%llu\n", (unsigned long long)frame.sequence);
	printf("timestamp_ns=%llu\n", (unsigned long long)frame.timestamp_ns);
	printf("framebuffer_id=%u\n", frame.framebuffer_id);
	printf("size=%ux%u\n", frame.width, frame.height);
	printf("format=0x%08x\n", frame.format);
	printf("modifier=0x%016llx\n", (unsigned long long)frame.modifier);
	printf("plane_count=%u\n", frame.plane_count);
	for (uint32_t i = 0; i < frame.plane_count && i < 4; i++) {
		printf("plane_%u_pitch=%u\n", i, frame.pitch[i]);
		printf("plane_%u_offset=%u\n", i, frame.offset[i]);
		printf("plane_%u_dma_buf_fd=%d\n", i, frame.dma_buf_fd[i]);
	}
	printf("sync_file_fd=%d\n", frame.sync_file_fd);

	if (require_dmabuf &&
	    !(frame.flags & HERMES_KMS_FRAME_DMABUF_VALID)) {
		fprintf(stderr, "ACQUIRE_FRAME did not provide the required DMA-BUF\n");
		ret = 1;
	}
	if (require_dmabuf &&
	    (!frame.plane_count || frame.plane_count > 4)) {
		fprintf(stderr, "ACQUIRE_FRAME returned invalid plane_count=%u\n",
			frame.plane_count);
		ret = 1;
	}
	if (require_dmabuf) {
		for (uint32_t i = 0; i < frame.plane_count && i < 4; i++) {
			if (frame.dma_buf_fd[i] < 0) {
				fprintf(stderr, "ACQUIRE_FRAME returned no fd for plane %u\n", i);
				ret = 1;
			}
		}
	}
	if (require_sync_file &&
	    (!(frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) ||
	     frame.sync_file_fd < 0)) {
		fprintf(stderr, "ACQUIRE_FRAME did not provide the requested sync_file\n");
		ret = 1;
	}
	for (uint32_t i = 0; i < 4; i++) {
		if (frame.dma_buf_fd[i] >= 0)
			close(frame.dma_buf_fd[i]);
	}
	if (frame.sync_file_fd >= 0)
		close(frame.sync_file_fd);

	return ret;
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
	printf("session_owned=%s\n",
	       (status.flags & HERMES_KMS_STATUS_SESSION_OWNED) ? "true" : "false");
	printf("hotplug_events=%s\n",
	       (status.flags & HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED) ? "true" : "false");
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
	printf("session_id=%llu\n",
	       (unsigned long long)status.session_id);
	printf("owner_pid=%d\n", status.owner_pid);
	printf("bound_fd_count=%llu\n",
	       (unsigned long long)status.bound_fd_count);
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

static int print_diagnose(int fd)
{
	struct drm_hermes_kms_status status;
	struct drm_hermes_kms_metrics metrics;
	bool have_metrics = true;

	memset(&status, 0, sizeof(status));
	memset(&metrics, 0, sizeof(metrics));

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) < 0) {
		perror("GET_STATUS");
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_METRICS, &metrics) < 0)
		have_metrics = false;

	printf("diagnostic_version=1\n");
	printf("enabled=%s\n",
	       (status.flags & HERMES_KMS_STATUS_OUTPUT_ENABLED) ? "true" : "false");
	printf("connected=%s\n",
	       (status.flags & HERMES_KMS_STATUS_CONNECTED) ? "true" : "false");
	printf("session_owned=%s\n",
	       (status.flags & HERMES_KMS_STATUS_SESSION_OWNED) ? "true" : "false");
	printf("hotplug_events=%s\n",
	       (status.flags & HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED) ? "true" : "false");
	printf("scanout_active=%s\n",
	       (status.flags & HERMES_KMS_STATUS_SCANOUT_ACTIVE) ? "true" : "false");
	printf("frame_valid=%s\n",
	       (status.flags & HERMES_KMS_STATUS_FRAME_VALID) ? "true" : "false");
	printf("dmabuf_export_ready=%s\n",
	       (status.flags & HERMES_KMS_STATUS_DMABUF_EXPORT_READY) ? "true" : "false");
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
	printf("framebuffer_id=%u\n", status.framebuffer_id);
	printf("owner_pid=%d\n", status.owner_pid);
	printf("bound_fd_count=%llu\n",
	       (unsigned long long)status.bound_fd_count);
	if (have_metrics) {
		printf("bind_count=%llu\n",
		       (unsigned long long)metrics.bind_count);
		printf("bind_reject_count=%llu\n",
		       (unsigned long long)metrics.bind_reject_count);
		printf("binding_revoke_count=%llu\n",
		       (unsigned long long)metrics.binding_revoke_count);
		printf("cross_session_buffer_export_count=%llu\n",
		       (unsigned long long)metrics.cross_session_buffer_export_count);
		printf("hotplug_event_count=%llu\n",
		       (unsigned long long)metrics.hotplug_event_count);
		printf("frame_update_count=%llu\n",
		       (unsigned long long)metrics.frame_update_count);
		printf("acquire_no_frame_count=%llu\n",
		       (unsigned long long)metrics.acquire_no_frame_count);
		printf("dmabuf_export_count=%llu\n",
		       (unsigned long long)metrics.dmabuf_export_count);
		printf("sync_file_export_count=%llu\n",
		       (unsigned long long)metrics.sync_file_export_count);
		printf("wait_timeout_count=%llu\n",
		       (unsigned long long)metrics.wait_timeout_count);
		printf("owner_close_disconnect_count=%llu\n",
		       (unsigned long long)metrics.owner_close_disconnect_count);
	} else {
		printf("metrics_available=false\n");
	}

	if (!(status.flags & HERMES_KMS_STATUS_OUTPUT_ENABLED)) {
		printf("summary=output_disabled\n");
		printf("next_step=Use hermes-kmsctl hold WIDTHxHEIGHT@HZ or let Hermes SET_OUTPUT before expecting a compositor framebuffer.\n");
	} else if (!(status.flags & HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED)) {
		printf("summary=hotplug_disabled\n");
		printf("next_step=Reload hermes_kms with hotplug_events=1 for compositor-driven sessions, or use modetest for isolated tests.\n");
	} else if (!(status.flags & HERMES_KMS_STATUS_SCANOUT_ACTIVE)) {
		printf("summary=no_atomic_modeset\n");
		printf("next_step=The connector is connected, but no compositor/modetest primary-plane commit is active yet.\n");
	} else if (!(status.flags & HERMES_KMS_STATUS_FRAME_VALID)) {
		printf("summary=no_framebuffer\n");
		printf("next_step=The CRTC is active, but the primary plane has no framebuffer; check the compositor/modetest -P commit.\n");
	} else if (!(status.flags & HERMES_KMS_STATUS_DMABUF_EXPORT_READY)) {
		printf("summary=frame_not_exportable\n");
		printf("next_step=The driver sees a framebuffer but cannot export DMA-BUF; inspect framebuffer format/modifier/import path.\n");
	} else {
		printf("summary=ready_for_acquire_frame\n");
		printf("next_step=Use hermes-kmsctl frame --require-dmabuf --sync-file or Hermes DMA-BUF import.\n");
	}

	return 0;
}

static bool parse_mode(const char *value, uint32_t *width, uint32_t *height,
			       uint32_t *refresh_hz)
{
	unsigned int parsed_width;
	unsigned int parsed_height;
	unsigned int parsed_refresh;
	char extra;

	if (sscanf(value, "%ux%u@%u%c", &parsed_width, &parsed_height,
		   &parsed_refresh, &extra) != 3 || !parsed_width ||
	    !parsed_height || !parsed_refresh)
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

	printf("enabled=%s\n", enabled ? "true" : "false");
	printf("applied=%ux%u@%u\n",
	       request.width,
	       request.height,
	       request.refresh_hz);
	printf("session_id=%llu\n", (unsigned long long)request.session_id);
	printf("result_flags=0x%08x\n", request.result_flags);
	printf("connected=%s\n",
	       (request.result_flags & HERMES_KMS_SET_OUTPUT_RESULT_CONNECTED) ? "true" : "false");
	printf("owner_assigned=%s\n",
	       (request.result_flags & HERMES_KMS_SET_OUTPUT_RESULT_OWNER_ASSIGNED) ? "true" : "false");
	printf("hotplug_sent=%s\n",
	       (request.result_flags & HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT) ? "true" : "false");

	return 0;
}

static void drop_master_if_held(int fd, bool verbose)
{
	if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && verbose)
		fprintf(stderr, "DROP_MASTER ignored: %s\n", strerror(errno));
}

/*
 * A private FIFO the holder reads commands from.
 *
 * The owner's authorization lives in the open file description that claimed the
 * output, not in the process or the uid, so a second hermes-kmsctl invocation
 * can never rotate or revoke on its behalf -- the driver compares owner_file
 * against the caller's drm_file and answers EACCES. The operation is only
 * reachable from inside the process holding that descriptor, which is this one.
 *
 * A FIFO rather than stdin: a backgrounded holder reading the terminal would
 * take SIGTTIN and stop, and a holder in a pipeline would eat input meant for
 * something else. Opening it O_RDWR keeps the reader alive across writers
 * coming and going, so `echo rotate > fifo` works any number of times.
 *
 * This channel revokes capture access, so it is held to the same standard as
 * the session file: a private FIFO owned by this uid, and nothing else.
 */
/*
 * Reject arguments a command does not take.
 *
 * Silently ignoring them turns a misplaced option into a run that looks
 * successful while doing something else: options have to precede the command,
 * so `hold 1280x720@60 --session-file X` held an output and published nothing,
 * with no indication that the flag had been dropped.
 */
static int reject_extra_arguments(const char *command, int count, char **values)
{
	if (count <= 0)
		return 0;

	fprintf(stderr, "%s does not take '%s'", command, values[0]);
	if (values[0][0] == '-')
		fprintf(stderr, "; options must come before the command");
	fprintf(stderr, "\n");
	return 2;
}

static int open_control_fifo(const char *path)
{
	struct stat status;
	int fd;

	if (mkfifo(path, S_IRUSR | S_IWUSR) < 0 && errno != EEXIST)
		return -1;

	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) < 0 || !S_ISFIFO(status.st_mode) ||
	    status.st_uid != geteuid() || (status.st_mode & 077) != 0) {
		close(fd);
		errno = EACCES;
		return -1;
	}

	return fd;
}

/*
 * Republish after a rotation, or clear after a revocation.
 *
 * Rotation keeps running consumers bound, so the file is replaced in place and
 * a reader sees either credential but never a torn one. Revocation cuts
 * everyone off, so the file is removed instead: leaving a working credential
 * behind would undo the thing that was just asked for. A later rotate
 * republishes.
 */
static int update_session_file(const char *session_file,
			       const struct hermes_session_credentials *credentials,
			       bool revoked)
{
	if (!session_file)
		return 0;

	if (revoked) {
		if (unlink(session_file) < 0 && errno != ENOENT) {
			fprintf(stderr, "Could not remove session file %s: %s\n",
				session_file, strerror(errno));
			return -1;
		}
		printf("session_file_removed=%s\n", session_file);
		fflush(stdout);
		return 0;
	}

	if (hermes_session_replace_file(session_file, credentials) < 0) {
		fprintf(stderr, "Could not republish session file %s: %s\n",
			session_file, strerror(errno));
		return -1;
	}
	printf("session_file_rotated=%s\n", session_file);
	fflush(stdout);
	return 0;
}

static void run_control_command(int fd, const char *command,
				const char *session_file)
{
	struct hermes_session_credentials credentials;
	bool revoke;

	if (!*command)
		return;
	if (strcmp(command, "rotate") == 0) {
		revoke = false;
	} else if (strcmp(command, "revoke") == 0) {
		revoke = true;
	} else {
		fprintf(stderr, "Unknown control command '%s'; expected rotate or revoke\n",
			command);
		return;
	}

	memset(&credentials, 0, sizeof(credentials));
	if (hermes_session_refresh_owner_token(fd, revoke, &credentials) < 0) {
		fprintf(stderr, "SESSION_ACCESS %s: %s\n",
			revoke ? "REVOKE_BINDINGS" : "ROTATE_TOKEN",
			strerror(errno));
		return;
	}
	printf("%s session=%llu\n", revoke ? "revoked" : "rotated",
	       (unsigned long long)credentials.session_id);
	fflush(stdout);
	update_session_file(session_file, &credentials, revoke);
	hermes_session_forget(&credentials);
}

/*
 * Drain whatever whole lines have arrived. A line longer than the buffer is a
 * malformed command, not a command to guess at, so it is discarded rather than
 * split into two.
 */
static void drain_control_fifo(int control_fd, int fd, const char *session_file)
{
	static char pending[256];
	static size_t used;
	static bool overlong;
	char buffer[256];
	ssize_t got;
	size_t i;

	for (;;) {
		got = read(control_fd, buffer, sizeof(buffer));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (got == 0)
			return;

		for (i = 0; i < (size_t)got; i++) {
			if (buffer[i] != '\n') {
				if (used < sizeof(pending) - 1)
					pending[used++] = buffer[i];
				else
					overlong = true;
				continue;
			}
			pending[used] = '\0';
			if (overlong)
				fprintf(stderr, "Discarding overlong control command\n");
			else
				run_control_command(fd, pending, session_file);
			used = 0;
			overlong = false;
		}
		if ((size_t)got < sizeof(buffer))
			return;
	}
}

static int hold_output(int fd, const char *mode, bool verbose,
			       const char *session_file,
			       const char *control_path)
{
	struct hermes_session_credentials credentials;
	struct sigaction action, old_int, old_term;
	sigset_t blocked_signals, original_mask, wait_mask;
	bool int_handler_installed = false;
	bool term_handler_installed = false;
	bool output_enabled = false;
	bool session_published = false;
	bool control_created = false;
	int control_fd = -1;
	int cleanup_ret;
	int ret = 1;

	sigemptyset(&blocked_signals);
	sigaddset(&blocked_signals, SIGINT);
	sigaddset(&blocked_signals, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &blocked_signals, &original_mask) < 0) {
		perror("sigprocmask");
		return 1;
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_stop_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &old_int) < 0) {
		perror("sigaction(SIGINT)");
		goto out_restore;
	}
	int_handler_installed = true;
	if (sigaction(SIGTERM, &action, &old_term) < 0) {
		perror("sigaction(SIGTERM)");
		goto out_restore;
	}
	term_handler_installed = true;

	if (control_path) {
		control_fd = open_control_fifo(control_path);
		if (control_fd < 0) {
			fprintf(stderr, "Could not open control FIFO %s: %s\n",
				control_path, strerror(errno));
			ret = 1;
			goto out_restore;
		}
		control_created = true;
	}

	ret = set_output(fd, true, mode);
	if (ret)
		goto out_restore;
	output_enabled = true;

	memset(&credentials, 0, sizeof(credentials));
	if (session_file) {
		if (hermes_session_get_owner_token(fd, &credentials) < 0) {
			perror("SESSION_ACCESS GET_TOKEN");
			ret = 1;
			goto out_disable;
		}
		if (hermes_session_write_file(session_file, &credentials) < 0) {
			fprintf(stderr, "Could not publish session file %s: %s\n",
				session_file, strerror(errno));
			hermes_session_forget(&credentials);
			ret = 1;
			goto out_disable;
		}
		session_published = true;
		printf("session_file=%s\n", session_file);
		hermes_session_forget(&credentials);
	}

	drop_master_if_held(fd, verbose);

	printf("holding Hermes-KMS output");
	if (mode)
		printf(" at %s", mode);
	printf("; press Ctrl+C to disconnect\n");
	if (control_path)
		printf("control_fifo=%s\n", control_path);
	fflush(stdout);

	wait_mask = original_mask;
	sigdelset(&wait_mask, SIGINT);
	sigdelset(&wait_mask, SIGTERM);
	while (!stop_requested) {
		struct pollfd control_poll;
		int ready;

		if (control_fd < 0) {
			sigsuspend(&wait_mask);
			continue;
		}
		control_poll.fd = control_fd;
		control_poll.events = POLLIN;
		control_poll.revents = 0;
		/*
		 * ppoll applies the unblocked mask atomically, so a signal that
		 * arrives just before the wait still ends it. sigsuspend gave
		 * the same guarantee before the control channel existed.
		 */
		ready = ppoll(&control_poll, 1, NULL, &wait_mask);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			perror("ppoll");
			break;
		}
		if (control_poll.revents & POLLIN)
			drain_control_fifo(control_fd, fd, session_file);
	}
	ret = 0;

out_disable:
	hermes_session_forget(&credentials);
	if (session_published && unlink(session_file) < 0 && errno != ENOENT) {
		fprintf(stderr, "Could not remove session file %s: %s\n",
			session_file, strerror(errno));
		if (!ret)
			ret = 1;
	}
	if (output_enabled) {
		cleanup_ret = set_output(fd, false, NULL);
		if (!ret && cleanup_ret)
			ret = cleanup_ret;
	}

out_restore:
	if (control_fd >= 0)
		close(control_fd);
	if (control_created && unlink(control_path) < 0 && errno != ENOENT) {
		fprintf(stderr, "Could not remove control FIFO %s: %s\n",
			control_path, strerror(errno));
		if (!ret)
			ret = 1;
	}
	if (sigprocmask(SIG_SETMASK, &original_mask, NULL) < 0) {
		perror("sigprocmask restore");
		if (!ret)
			ret = 1;
	}
	if (term_handler_installed)
		(void)sigaction(SIGTERM, &old_term, NULL);
	if (int_handler_installed)
		(void)sigaction(SIGINT, &old_int, NULL);
	return ret;
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	const char *session_file = NULL;
	const char *control_path = NULL;
	const char *command;
	uint32_t output_number = 1;
	bool output_selected = false;
	int argi = 1;
	int fd;
	int ret;
	bool verbose = false;
	bool session_bound = false;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
		if (strcmp(argv[argi], "--device") == 0) {
			if (argi + 1 >= argc) {
				fprintf(stderr, "--device requires a path\n");
				return 2;
			}
			device = argv[argi + 1];
			argi += 2;
		} else if (strcmp(argv[argi], "--output") == 0) {
			if (argi + 1 >= argc ||
			    parse_u32(argv[argi + 1], &output_number) < 0 ||
			    !output_number) {
				fprintf(stderr,
					"--output requires a positive 1-based number\n");
				return 2;
			}
			output_selected = true;
			argi += 2;
		} else if (strcmp(argv[argi], "--verbose") == 0) {
			verbose = true;
			argi++;
		} else if (strcmp(argv[argi], "--control") == 0) {
			if (argi + 1 >= argc) {
				fprintf(stderr, "--control requires a path\n");
				return 2;
			}
			control_path = argv[argi + 1];
			argi += 2;
		} else if (strcmp(argv[argi], "--session-file") == 0) {
			if (argi + 1 >= argc) {
				fprintf(stderr, "--session-file requires a path\n");
				return 2;
			}
			session_file = argv[argi + 1];
			argi += 2;
		} else {
			fprintf(stderr, "Unknown option '%s'\n", argv[argi]);
			usage(argv[0]);
			return 2;
		}
	}

	if (argi >= argc) {
		usage(argv[0]);
		return 2;
	}

	command = argv[argi++];
	if (session_file && !device && strcmp(command, "hold") != 0 &&
	    strcmp(command, "enable") != 0) {
		char bound_device[PATH_MAX];
		uint32_t bound_output;

		fd = hermes_session_open_bound_render(session_file, bound_device,
						     sizeof(bound_device),
						     &bound_output);
		if (fd >= 0) {
			session_bound = true;
			if (verbose)
				fprintf(stderr, "session bound to %s\n", bound_device);
			if (output_selected && bound_output != output_number - 1) {
				fprintf(stderr,
					"--output %u does not match session file output %u\n",
					output_number, bound_output + 1);
				close(fd);
				return 2;
			}
			output_number = bound_output + 1;
			output_selected = true;
		}
	} else {
		fd = open_device(device, verbose);
	}
	if (fd < 0) {
		fprintf(stderr, "Could not find/open or bind a Hermes-KMS DRM device");
		if (device)
			fprintf(stderr, " at %s", device);
		fprintf(stderr, "\n");
		return 1;
	}

	if (session_file && !session_bound && strcmp(command, "hold") != 0 &&
	    strcmp(command, "enable") != 0) {
		uint32_t bound_output;

		if (hermes_session_bind_file(fd, session_file, &bound_output) < 0) {
			fprintf(stderr, "Could not bind session file %s: %s\n",
				session_file, strerror(errno));
			close(fd);
			return 1;
		}
		if (output_selected && bound_output != output_number - 1) {
			fprintf(stderr,
				"--output %u does not match session file output %u\n",
				output_number, bound_output + 1);
			close(fd);
			return 2;
		}
		output_number = bound_output + 1;
		output_selected = true;
	} else if (output_selected && select_output(fd, output_number)) {
		close(fd);
		return 1;
	}

	if (strcmp(command, "wait") == 0) {
		ret = wait_frame(fd, argc - argi, &argv[argi]);
	} else if (strcmp(command, "frame") == 0) {
		ret = print_frame(fd, argc - argi, &argv[argi]);
	} else if (strcmp(command, "enable") == 0 ||
		   strcmp(command, "hold") == 0) {
		const char *mode = argi < argc ? argv[argi++] : NULL;

		ret = reject_extra_arguments(command, argc - argi, &argv[argi]);
		if (!ret) {
			if (strcmp(command, "enable") == 0)
				fprintf(stderr,
					"note: output ownership is fd-scoped; holding until Ctrl+C\n");
			ret = hold_output(fd, mode, verbose, session_file,
					  control_path);
		}
	} else if ((ret = reject_extra_arguments(command, argc - argi,
						 &argv[argi])) != 0) {
		/* Reported above. */
	} else if (strcmp(command, "version") == 0) {
		ret = print_version(fd);
	} else if (strcmp(command, "outputs") == 0) {
		ret = print_outputs(fd);
	} else if (strcmp(command, "identity") == 0) {
		ret = print_identity(fd);
	} else if (strcmp(command, "caps") == 0) {
		ret = print_caps(fd);
	} else if (strcmp(command, "status") == 0) {
		ret = print_status(fd);
	} else if (strcmp(command, "metrics") == 0) {
		ret = print_metrics(fd);
	} else if (strcmp(command, "diagnose") == 0) {
		ret = print_diagnose(fd);
	} else if (strcmp(command, "disable") == 0) {
		ret = set_output(fd, false, NULL);
	} else {
		usage(argv[0]);
		ret = 2;
	}

	close(fd);
	return ret;
}
