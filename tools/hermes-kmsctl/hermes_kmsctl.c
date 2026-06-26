// SPDX-License-Identifier: GPL-2.0

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
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
		"  %s [--device /dev/dri/cardN] [--verbose] version\n"
		"  %s [--device /dev/dri/cardN] [--verbose] identity\n"
		"  %s [--device /dev/dri/cardN] [--verbose] caps\n"
		"  %s [--device /dev/dri/cardN] [--verbose] status\n"
		"  %s [--device /dev/dri/cardN] [--verbose] metrics\n"
		"  %s [--device /dev/dri/cardN] [--verbose] diagnose\n"
		"  %s [--device /dev/dri/cardN] [--verbose] wait [AFTER_SEQUENCE] [TIMEOUT_MS]\n"
		"  %s [--device /dev/dri/cardN] [--verbose] frame [--require-dmabuf] [--sync-file]\n"
		"  %s [--device /dev/dri/cardN] [--verbose] enable [WIDTHxHEIGHT@HZ]\n"
		"  %s [--device /dev/dri/cardN] [--verbose] hold [WIDTHxHEIGHT@HZ]\n"
		"  %s [--device /dev/dri/cardN] [--verbose] disable\n",
		argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version) == 0 &&
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
	int fd = -1;
	int pass;

	dir = opendir("/dev/dri");
	if (!dir) {
		if (verbose)
			fprintf(stderr, "/dev/dri: open failed: %s\n", strerror(errno));
		return -1;
	}

	for (pass = 0; pass < 2 && fd < 0; pass++) {
		rewinddir(dir);

		while ((entry = readdir(dir)) != NULL) {
			char path[PATH_MAX];

			if (pass == 0) {
				if (strncmp(entry->d_name, "renderD", 7) != 0)
					continue;
			} else {
				if (strncmp(entry->d_name, "card", 4) != 0)
					continue;
			}

			snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
			fd = open_if_hermes(path, verbose);
			if (fd >= 0)
				break;
		}
	}

	closedir(dir);
	return fd;
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

	return 0;
}

static int parse_u64(const char *value, uint64_t *out)
{
	char *end;
	unsigned long long parsed;

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

	return 0;
}

static int print_frame(int fd, int argc, char **argv)
{
	struct drm_hermes_kms_acquire_frame frame;

	memset(&frame, 0, sizeof(frame));

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--require-dmabuf") == 0)
			frame.flags |= HERMES_KMS_FRAME_REQUEST_DMABUF;
		else if (strcmp(argv[i], "--sync-file") == 0)
			frame.flags |= HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
		else {
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
	if (have_metrics) {
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

static int hold_output(int fd, const char *mode, bool verbose)
{
	struct sigaction action;
	int ret;

	ret = set_output(fd, true, mode);
	if (ret)
		return ret;

	drop_master_if_held(fd, verbose);

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_stop_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	printf("holding Hermes-KMS output");
	if (mode)
		printf(" at %s", mode);
	printf("; press Ctrl+C to disconnect\n");
	fflush(stdout);

	while (!stop_requested)
		pause();

	return set_output(fd, false, NULL);
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	const char *command;
	int argi = 1;
	int fd;
	int ret;
	bool verbose = false;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (argc >= 4 && strcmp(argv[argi], "--device") == 0) {
		device = argv[argi + 1];
		argi += 2;
	}

	if (argi < argc && strcmp(argv[argi], "--verbose") == 0) {
		verbose = true;
		argi++;
	}

	if (argi >= argc) {
		usage(argv[0]);
		return 2;
	}

	command = argv[argi++];
	fd = open_device(device, verbose);
	if (fd < 0) {
		fprintf(stderr, "Could not find/open Hermes-KMS DRM device");
		if (device)
			fprintf(stderr, " at %s", device);
		fprintf(stderr, "\n");
		return 1;
	}

	if (strcmp(command, "version") == 0)
		ret = print_version(fd);
	else if (strcmp(command, "identity") == 0)
		ret = print_identity(fd);
	else if (strcmp(command, "caps") == 0)
		ret = print_caps(fd);
	else if (strcmp(command, "status") == 0)
		ret = print_status(fd);
	else if (strcmp(command, "metrics") == 0)
		ret = print_metrics(fd);
	else if (strcmp(command, "diagnose") == 0)
		ret = print_diagnose(fd);
	else if (strcmp(command, "wait") == 0)
		ret = wait_frame(fd, argc - argi, &argv[argi]);
	else if (strcmp(command, "frame") == 0)
		ret = print_frame(fd, argc - argi, &argv[argi]);
	else if (strcmp(command, "enable") == 0)
		ret = set_output(fd, true, argi < argc ? argv[argi] : NULL);
	else if (strcmp(command, "hold") == 0)
		ret = hold_output(fd, argi < argc ? argv[argi] : NULL, verbose);
	else if (strcmp(command, "disable") == 0)
		ret = set_output(fd, false, NULL);
	else {
		usage(argv[0]);
		ret = 2;
	}

	close(fd);
	return ret;
}
