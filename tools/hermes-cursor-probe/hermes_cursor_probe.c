/* hermes_cursor_probe.c - inspect KMS and generic cursor-capture state.
 *
 * Polls the hermes-kms DRM planes and reports, for the cursor plane:
 *   FB_ID, CRTC_ID, SRC_X/SRC_Y (16.16 fixed), CRTC_X/CRTC_Y
 * and, for the primary plane: FB_ID (the framebuffer the capture sees).
 *
 * Interpretation:
 *   - cursor plane FB_ID != 0 and changing with pointer motion:
 *       the compositor is using the HW cursor plane. The pointer is NOT
 *       drawn into the primary framebuffer. A capture consumer must combine
 *       the primary and the independent ACQUIRE_CURSOR stream.
 *   - cursor plane FB_ID == 0 while moving the pointer:
 *       the compositor is drawing the cursor itself (software cursor) into
 *       the primary framebuffer; cursor pixels ARE in the captured frame.
 *
 * Build: cc $(pkg-config --cflags libdrm) -o hermes_cursor_probe \
 *            hermes_cursor_probe.c $(pkg-config --libs libdrm)
 *
 * With --session-file, the probe also binds an authorized render fd and
 * validates WAIT_UPDATE, ACQUIRE_CURSOR, sync_file and DMA-BUF mmap at runtime.
 *
 * Usage: hermes_cursor_probe [--device /dev/dri/cardN]
 *                            [--session-file PATH] [--interval-ms N]
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../hermes_session.h"

#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

struct plane_info {
	uint32_t id;
	uint32_t type;
	uint32_t prop_fb_id;
	uint32_t prop_crtc_id;
	uint32_t prop_src_x;
	uint32_t prop_src_y;
	uint32_t prop_crtc_x;
	uint32_t prop_crtc_y;
};

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
			  const char *name)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		return 0;
	}
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			uint32_t ret = props->props[i];
			drmModeFreeProperty(p);
			drmModeFreeObjectProperties(props);
			return ret;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return 0;
}

static bool parse_interval(const char *text, int *interval_ms)
{
	char *end = NULL;
	long parsed;

	if (!text || !*text || *text == '-')
		return false;
	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || !end || *end || parsed < 1 || parsed > 60000)
		return false;
	*interval_ms = (int)parsed;
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

static int open_device(const char *requested, char *discovered,
		       size_t discovered_size)
{
	if (requested)
		return open(requested, O_RDWR | O_CLOEXEC);

	for (unsigned int index = 0; index < 64; index++) {
		int length = snprintf(discovered, discovered_size,
				      "/dev/dri/card%u", index);
		int fd;

		if (length < 0 || (size_t)length >= discovered_size)
			break;
		fd = open(discovered, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (is_hermes(fd))
			return fd;
		close(fd);
	}
	errno = ENODEV;
	return -1;
}

static uint64_t get_prop(int fd, uint32_t obj_id, uint32_t obj_type, uint32_t prop_id)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		return 0;
	}
	for (uint32_t i = 0; i < props->count_props; i++) {
		if (props->props[i] == prop_id) {
			uint64_t v = props->prop_values[i];
			drmModeFreeObjectProperties(props);
			return v;
		}
	}
	drmModeFreeObjectProperties(props);
	return 0;
}

static void close_cursor_fds(struct drm_hermes_kms_acquire_cursor *cursor)
{
	for (size_t i = 0; i < sizeof(cursor->dma_buf_fd) /
					  sizeof(cursor->dma_buf_fd[0]); i++) {
		if (cursor->dma_buf_fd[i] >= 0) {
			close(cursor->dma_buf_fd[i]);
			cursor->dma_buf_fd[i] = -1;
		}
	}
	if (cursor->sync_file_fd >= 0) {
		close(cursor->sync_file_fd);
		cursor->sync_file_fd = -1;
	}
}

static int dma_buf_read_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };
	int ret;

	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static int checksum_cursor_buffer(struct drm_hermes_kms_acquire_cursor *cursor,
				  uint32_t *checksum,
				  uint64_t *nonzero_pixels,
				  uint64_t *alpha_pixels)
{
	struct stat status = {0};
	const size_t width = cursor->width;
	const size_t height = cursor->height;
	const size_t pitch = cursor->pitch[0];
	const size_t offset = cursor->offset[0];
	size_t row_bytes;
	size_t last_row_offset;
	size_t map_length;
	void *mapping;
	uint32_t hash = 2166136261U;
	uint64_t nonzero_count = 0;
	uint64_t alpha_count = 0;
	int sync_errno;
	int ret = -1;

	if (cursor->plane_count != 1 || cursor->dma_buf_fd[0] < 0 ||
	    cursor->format != DRM_FORMAT_ARGB8888 || !width || !height ||
	    (cursor->modifier != DRM_FORMAT_MOD_LINEAR &&
	     cursor->modifier != DRM_FORMAT_MOD_INVALID) ||
	    width > SIZE_MAX / 4) {
		errno = EPROTO;
		return -1;
	}
	row_bytes = width * 4;
	if (pitch < row_bytes || height - 1 > (SIZE_MAX - offset) / pitch) {
		errno = EOVERFLOW;
		return -1;
	}
	last_row_offset = offset + (height - 1) * pitch;
	if (row_bytes > SIZE_MAX - last_row_offset) {
		errno = EOVERFLOW;
		return -1;
	}
	map_length = last_row_offset + row_bytes;
	if (fstat(cursor->dma_buf_fd[0], &status) < 0)
		return -1;
	if (status.st_size <= 0 ||
	    (uintmax_t)map_length > (uintmax_t)status.st_size) {
		errno = EPROTO;
		return -1;
	}

	mapping = mmap(NULL, map_length, PROT_READ, MAP_SHARED,
		       cursor->dma_buf_fd[0], 0);
	if (mapping == MAP_FAILED)
		return -1;
	if (dma_buf_read_sync(cursor->dma_buf_fd[0],
			       DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0)
		goto out_unmap;

	for (size_t y = 0; y < height; y++) {
		const unsigned char *row = (const unsigned char *)mapping +
			offset + y * pitch;

		for (size_t x = 0; x < width; x++) {
			uint32_t pixel;

			memcpy(&pixel, row + x * sizeof(pixel), sizeof(pixel));
			if (pixel)
				nonzero_count++;
			if (pixel & UINT32_C(0xff000000))
				alpha_count++;
			for (size_t byte = 0; byte < sizeof(pixel); byte++) {
				hash ^= row[x * sizeof(pixel) + byte];
				hash *= 16777619U;
			}
		}
	}
	if (dma_buf_read_sync(cursor->dma_buf_fd[0],
			       DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0)
		goto out_unmap;
	*checksum = hash;
	*nonzero_pixels = nonzero_count;
	*alpha_pixels = alpha_count;
	ret = 0;

out_unmap:
	sync_errno = errno;
	if (munmap(mapping, map_length) < 0 && !ret)
		return -1;
	if (ret)
		errno = sync_errno;
	return ret;
}

static int poll_cursor_uapi(int fd, uint64_t *last_frame_sequence,
			    uint64_t *last_cursor_sequence)
{
	struct drm_hermes_kms_wait_update wait = {0};
	struct drm_hermes_kms_acquire_cursor cursor = {0};
	const uint64_t ready_mask = HERMES_KMS_WAIT_UPDATE_FRAME_READY |
		HERMES_KMS_WAIT_UPDATE_CURSOR_READY;
	const uint64_t cursor_result_mask =
		HERMES_KMS_CURSOR_METADATA_VALID | HERMES_KMS_CURSOR_VISIBLE |
		HERMES_KMS_CURSOR_POSITION_VALID | HERMES_KMS_CURSOR_HOTSPOT_VALID |
		HERMES_KMS_CURSOR_BUFFER_VALID | HERMES_KMS_CURSOR_DMABUF_VALID |
		HERMES_KMS_CURSOR_SYNC_FILE_VALID | HERMES_KMS_CURSOR_GEOMETRY_VALID;
	uint32_t checksum = 0;
	uint64_t nonzero_pixels = 0;
	uint64_t alpha_pixels = 0;
	int ret;

	wait.after_frame_sequence = *last_frame_sequence;
	wait.after_cursor_sequence = *last_cursor_sequence;
	do {
		ret = ioctl(fd, DRM_IOCTL_HERMES_KMS_WAIT_UPDATE, &wait);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		if (errno == EAGAIN)
			return 0;
		perror("WAIT_UPDATE");
		return -1;
	}
	if (!(wait.flags & ready_mask) || (wait.flags & ~ready_mask) ||
	    !!(wait.flags & HERMES_KMS_WAIT_UPDATE_FRAME_READY) !=
		(wait.frame_sequence > *last_frame_sequence) ||
	    !!(wait.flags & HERMES_KMS_WAIT_UPDATE_CURSOR_READY) !=
		(wait.cursor_sequence > *last_cursor_sequence)) {
		fprintf(stderr, "WAIT_UPDATE returned incoherent flags/sequences\n");
		errno = EPROTO;
		return -1;
	}
	*last_frame_sequence = wait.frame_sequence;
	if (!(wait.flags & HERMES_KMS_WAIT_UPDATE_CURSOR_READY))
		return 0;

	cursor.flags = HERMES_KMS_CURSOR_REQUEST_DMABUF |
		HERMES_KMS_CURSOR_REQUEST_SYNC_FILE;
	for (size_t i = 0; i < sizeof(cursor.dma_buf_fd) /
					  sizeof(cursor.dma_buf_fd[0]); i++)
		cursor.dma_buf_fd[i] = -1;
	cursor.sync_file_fd = -1;
	for (unsigned int attempt = 0; attempt < 4; attempt++) {
		do {
			ret = ioctl(fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_CURSOR,
				    &cursor);
		} while (ret < 0 && errno == EINTR);
		if (!ret || errno != ESTALE)
			break;
		memset(&cursor, 0, sizeof(cursor));
		cursor.flags = HERMES_KMS_CURSOR_REQUEST_DMABUF |
			HERMES_KMS_CURSOR_REQUEST_SYNC_FILE;
		for (size_t i = 0; i < sizeof(cursor.dma_buf_fd) /
						  sizeof(cursor.dma_buf_fd[0]); i++)
			cursor.dma_buf_fd[i] = -1;
		cursor.sync_file_fd = -1;
	}
	if (ret < 0) {
		perror("ACQUIRE_CURSOR");
		return -1;
	}
	if (!(cursor.flags & HERMES_KMS_CURSOR_METADATA_VALID) ||
	    (cursor.flags & ~(cursor_result_mask)) ||
	    cursor.sequence < wait.cursor_sequence) {
		fprintf(stderr, "ACQUIRE_CURSOR returned malformed metadata\n");
		close_cursor_fds(&cursor);
		errno = EPROTO;
		return -1;
	}

	if (cursor.flags & HERMES_KMS_CURSOR_BUFFER_VALID) {
		if (!(cursor.flags & HERMES_KMS_CURSOR_DMABUF_VALID) ||
		    !(cursor.flags & HERMES_KMS_CURSOR_SYNC_FILE_VALID) ||
		    cursor.sync_file_fd < 0 ||
		    hermes_sync_file_wait(cursor.sync_file_fd, 1000) < 0 ||
		    checksum_cursor_buffer(&cursor, &checksum, &nonzero_pixels,
					   &alpha_pixels) < 0) {
			perror("cursor buffer validation");
			close_cursor_fds(&cursor);
			return -1;
		}
	}
	*last_cursor_sequence = cursor.sequence;
	printf("[uapi] frame_seq=%" PRIu64 " cursor_seq=%" PRIu64
	       " image_seq=%" PRIu64 " flags=0x%" PRIx64
	       " wait_flags=0x%" PRIx64 " frame_ready=%d cursor_ready=%d"
	       " cursor_only=%d visible=%d position_valid=%d hotspot_valid=%d"
	       " geometry_valid=%d buffer_valid=%d position=(%d,%d)"
	       " hotspot=(%d,%d) clipped=(%d,%d %ux%u) checksum=0x%08x"
	       " nonzero_pixels=%" PRIu64 " alpha_pixels=%" PRIu64 "\n",
	       (uint64_t)wait.frame_sequence, (uint64_t)cursor.sequence,
	       (uint64_t)cursor.image_sequence, (uint64_t)cursor.flags,
	       (uint64_t)wait.flags,
	       !!(wait.flags & HERMES_KMS_WAIT_UPDATE_FRAME_READY),
	       !!(wait.flags & HERMES_KMS_WAIT_UPDATE_CURSOR_READY),
	       !(wait.flags & HERMES_KMS_WAIT_UPDATE_FRAME_READY),
	       !!(cursor.flags & HERMES_KMS_CURSOR_VISIBLE),
	       !!(cursor.flags & HERMES_KMS_CURSOR_POSITION_VALID),
	       !!(cursor.flags & HERMES_KMS_CURSOR_HOTSPOT_VALID),
	       !!(cursor.flags & HERMES_KMS_CURSOR_GEOMETRY_VALID),
	       !!(cursor.flags & HERMES_KMS_CURSOR_BUFFER_VALID),
	       cursor.position_x, cursor.position_y,
	       cursor.hotspot_x, cursor.hotspot_y,
	       cursor.crtc_x, cursor.crtc_y, cursor.crtc_w, cursor.crtc_h,
	       checksum, nonzero_pixels, alpha_pixels);
	close_cursor_fds(&cursor);
	return 1;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *dev = NULL;
	const char *session_file = NULL;
	char discovered[PATH_MAX];
	char capture_device[PATH_MAX];
	int interval_ms = 250;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "--session-file") && i + 1 < argc) {
			session_file = argv[++i];
		} else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
			if (!parse_interval(argv[++i], &interval_ms)) {
				fprintf(stderr, "--interval-ms expects an integer from 1 to 60000\n");
				return 1;
			}
		} else {
			fprintf(stderr,
				"usage: %s [--device /dev/dri/cardN] "
				"[--session-file PATH] [--interval-ms N]\n",
				argv[0]);
			return 1;
		}
	}

	int fd = open_device(dev, discovered, sizeof(discovered));
	if (fd < 0) {
		perror(dev ? "open" : "auto-discover hermes-kms card");
		return 1;
	}
	if (!dev)
		dev = discovered;
	if (!is_hermes(fd)) {
		fprintf(stderr, "%s is not a hermes-kms card\n", dev);
		close(fd);
		return 1;
	}
	struct drm_hermes_kms_identity card_identity = {0};
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY, &card_identity) < 0) {
		perror("GET_IDENTITY on card");
		close(fd);
		return 1;
	}
	int capture_fd = -1;
	if (session_file) {
		struct drm_hermes_kms_identity capture_identity = {0};
		uint32_t bound_output_index = UINT32_MAX;

		capture_fd = hermes_session_open_bound_render(
			session_file, capture_device, sizeof(capture_device), NULL);
		if (capture_fd < 0) {
			perror("open authorized cursor capture");
			close(fd);
			return 1;
		}
		if (ioctl(capture_fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY,
			  &capture_identity) < 0) {
			perror("GET_IDENTITY on capture fd");
			close(capture_fd);
			close(fd);
			return 1;
		}
		if (capture_identity.device_index != card_identity.device_index) {
			fprintf(stderr,
				"session belongs to Hermes device %u, but %s is device %u\n",
				capture_identity.device_index + 1, dev,
				card_identity.device_index + 1);
			close(capture_fd);
			close(fd);
			return 1;
		}
		if (hermes_session_bind_file(fd, session_file,
					     &bound_output_index) < 0) {
			perror("bind card fd to matching session");
			close(capture_fd);
			close(fd);
			return 1;
		}
		if (bound_output_index != capture_identity.output_index ||
		    ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_IDENTITY,
			  &card_identity) < 0) {
			fprintf(stderr,
				"card fd selected a different output after session bind\n");
			close(capture_fd);
			close(fd);
			return 1;
		}
		if (card_identity.plane_id != capture_identity.plane_id ||
		    card_identity.cursor_plane_id != capture_identity.cursor_plane_id ||
		    card_identity.crtc_id != capture_identity.crtc_id) {
			fprintf(stderr,
				"card and capture identities disagree after output selection\n");
			close(capture_fd);
			close(fd);
			return 1;
		}
		printf("capture render node: %s\n", capture_device);
	}
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		perror("drmSetClientCap");
		if (capture_fd >= 0)
			close(capture_fd);
		close(fd);
		return 1;
	}

	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		perror("drmModeGetPlaneResources");
		if (capture_fd >= 0)
			close(capture_fd);
		close(fd);
		return 1;
	}

	struct plane_info primary = {0}, cursor = {0};

	for (uint32_t i = 0; i < planes->count_planes; i++) {
		drmModePlanePtr p = drmModeGetPlane(fd, planes->planes[i]);
		if (!p) {
			continue;
		}
		uint32_t type_prop = find_prop(fd, p->plane_id,
					       DRM_MODE_OBJECT_PLANE, "type");
		uint64_t type = type_prop ? get_prop(fd, p->plane_id,
						    DRM_MODE_OBJECT_PLANE,
						    type_prop) : 0;

		if (type == DRM_PLANE_TYPE_PRIMARY &&
		    p->plane_id == card_identity.plane_id) {
			primary.id = p->plane_id;
			primary.type = (uint32_t)type;
			primary.prop_fb_id = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			primary.prop_crtc_id = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
		} else if (type == DRM_PLANE_TYPE_CURSOR &&
			   p->plane_id == card_identity.cursor_plane_id) {
			cursor.id = p->plane_id;
			cursor.type = (uint32_t)type;
			cursor.prop_fb_id = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			cursor.prop_crtc_id = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
			cursor.prop_src_x = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
			cursor.prop_src_y = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
			cursor.prop_crtc_x = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
			cursor.prop_crtc_y = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);

	printf("device: %s\n", dev);
	printf("primary plane: id=%u fb_id_prop=%u crtc_id_prop=%u\n",
	       primary.id, primary.prop_fb_id, primary.prop_crtc_id);
	printf("cursor plane:  id=%u fb_id_prop=%u crtc_id_prop=%u src_x=%u src_y=%u crtc_x=%u crtc_y=%u\n",
	       cursor.id, cursor.prop_fb_id, cursor.prop_crtc_id,
	       cursor.prop_src_x, cursor.prop_src_y, cursor.prop_crtc_x, cursor.prop_crtc_y);
	if (!primary.id || !cursor.id) {
		fprintf(stderr,
			"selected output's primary/cursor plane was not exposed "
			"(expected %u/%u)\n",
			card_identity.plane_id, card_identity.cursor_plane_id);
		if (capture_fd >= 0)
			close(capture_fd);
		close(fd);
		return 1;
	}

	uint64_t last_pfb = ~0ULL, last_cfb = ~0ULL, last_crtc = ~0ULL;
	int64_t last_sx = -1, last_sy = -1;
	int64_t last_cx = INT64_MIN, last_cy = INT64_MIN;
	uint64_t heartbeat = 0;
	uint64_t last_frame_sequence = 0;
	uint64_t last_cursor_sequence = 0;

	for (;;) {
		if (capture_fd >= 0 &&
		    poll_cursor_uapi(capture_fd, &last_frame_sequence,
				     &last_cursor_sequence) < 0) {
			close(capture_fd);
			close(fd);
			return 1;
		}
		uint64_t pfb = primary.id ? get_prop(fd, primary.id, DRM_MODE_OBJECT_PLANE, primary.prop_fb_id) : 0;
		uint64_t cfb = cursor.id ? get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_fb_id) : 0;
		uint64_t ccrtc = cursor.id ? get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_crtc_id) : 0;
		int64_t sx = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_src_x) : -1;
		int64_t sy = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_src_y) : -1;
		int64_t cx = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_crtc_x) : -1;
		int64_t cy = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_crtc_y) : -1;

		int changed = (pfb != last_pfb) || (cfb != last_cfb) ||
			      (ccrtc != last_crtc) || (sx != last_sx) ||
			      (sy != last_sy) || (cx != last_cx) || (cy != last_cy);
		if (changed) {
			printf("[change] primary_fb=%" PRIu64 " cursor_fb=%" PRIu64 " cursor_crtc=%" PRIu64
			       " cursor_src=(%d.%04d, %d.%04d) cursor_crtc_pos=(%" PRId64 ",%" PRId64 ")\n",
			       pfb, cfb, ccrtc,
			       (int)(sx >> 16), (int)((sx & 0xFFFF) * 10000 / 65536),
			       (int)(sy >> 16), (int)((sy & 0xFFFF) * 10000 / 65536),
			       cx, cy);
			last_pfb = pfb;
			last_cfb = cfb;
			last_crtc = ccrtc;
			last_sx = sx;
			last_sy = sy;
			last_cx = cx;
			last_cy = cy;
		} else if (++heartbeat % 8 == 0) {
			printf("[same]   primary_fb=%" PRIu64 " cursor_fb=%" PRIu64 " cursor_crtc=%" PRIu64 "\n",
			       pfb, cfb, ccrtc);
		}

		usleep((useconds_t)interval_ms * 1000);
	}

	close(fd);
	return 0;
}
