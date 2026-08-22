/* hermes_pixel_peek.c - read pixels out of the hermes-kms captured framebuffer.
 *
 * Closes the "no easy path to the bytes" gap: ACQUIRE_FRAME with
 * FRAME_REQUEST_DMABUF | FRAME_REQUEST_SYNC_FILE exports the tracked scanout
 * framebuffer as a DMA-BUF (shmem-backed, so it is mmappable), and the
 * sync_file is the producer write fence. This tool follows new frames
 * (WAIT_FRAME), waits on the fence, mmaps the buffer and dumps a region.
 *
 * Use it to answer "does the compositor draw the cursor into the primary
 * framebuffer?": run it with a region where the pointer is, move the pointer
 * over that region and watch the checksum/pixels change (or not).
 *
 * Build: cc -I<repo>/include/uapi -o hermes_pixel_peek hermes_pixel_peek.c
 *
 * Usage: hermes_pixel_peek --session-file PATH
 *                          [--device /dev/dri/renderDN] [--region x,y,w,h]
 *                          [--frames N] [--no-dmabuf]
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <drm/drm.h>
#include <drm/hermes_kms_drm.h>

#include "../hermes_session.h"

#define XR24 0x34325258 /* DRM_FORMAT_XRGB8888 little-endian fourcc */
#define AR24 0x34325241 /* DRM_FORMAT_ARGB8888 */

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *dev = NULL;
	const char *session_file = NULL;
	long frames = 0; /* 0 = until interrupted */
	int no_dmabuf = 0;
	int has_region = 0;
	uint32_t rx = 0, ry = 0, rw = 0, rh = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "--session-file") && i + 1 < argc) {
			session_file = argv[++i];
		} else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
			char extra;

			if (sscanf(argv[++i], "%u,%u,%u,%u%c", &rx, &ry, &rw, &rh,
				   &extra) != 4) {
				fprintf(stderr, "--region expects x,y,w,h\n");
				return 1;
			}
			has_region = 1;
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			char *end = NULL;

			errno = 0;
			frames = strtol(argv[++i], &end, 10);
			if (errno || !end || *end || frames <= 0) {
				fprintf(stderr, "--frames expects a positive integer\n");
				return 1;
			}
		} else if (!strcmp(argv[i], "--no-dmabuf")) {
			no_dmabuf = 1;
		} else {
			fprintf(stderr,
			        "usage: %s [--device /dev/dri/renderDN] [--session-file PATH] [--region x,y,w,h] [--frames N] [--no-dmabuf]\n",
			        argv[0]);
			return 1;
		}
	}

	if (!session_file) {
		fprintf(stderr,
			"--session-file is required for capture on secure UAPI v11 sessions\n");
		return 1;
	}

	char discovered_device[256];
	int already_bound = 0;
	int fd;

	if (dev) {
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			perror("open");
			return 1;
		}
	} else {
		fd = hermes_session_open_bound_render(session_file,
						     discovered_device,
						     sizeof(discovered_device), NULL);
		if (fd < 0) {
			fprintf(stderr, "could not find the render node for session %s: %s\n",
				session_file, strerror(errno));
			return 1;
		}
		dev = discovered_device;
		already_bound = 1;
	}

	if (!already_bound && hermes_session_require_token_uapi(fd) < 0) {
		fprintf(stderr, "%s is not a Hermes-KMS v11 session device: %s\n",
			dev, strerror(errno));
		close(fd);
		return 1;
	}
	struct drm_hermes_kms_version ver = {0};
	if (ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &ver) != 0) {
		perror("GET_VERSION (is this a hermes-kms device?)");
		close(fd);
		return 1;
	}
	printf("device: %s\ndriver: %s %u.%u.%u uapi=%u\n", dev, ver.driver_name, ver.driver_major,
	       ver.driver_minor, ver.driver_patch, ver.uapi_version);
	if (!already_bound && hermes_session_bind_file(fd, session_file, NULL) < 0) {
		fprintf(stderr, "SESSION_ACCESS BIND failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	uint64_t after_seq = 0;
	uint32_t last_checksum = 0;
	long count = 0;

	while (frames == 0 || count < frames) {
		struct drm_hermes_kms_wait_frame wait = {0};
		wait.after_sequence = after_seq;
		wait.timeout_ms = 1000;
		int new_frame = 1;
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) != 0) {
			if (errno == ETIMEDOUT) {
				/* No new frame: sample the current scanout anyway, so a
				 * pointer moving on the cursor plane (which does not
				 * dirty the primary FB) is still observable. */
				new_frame = 0;
			} else {
				perror("WAIT_FRAME");
				return 1;
			}
		}

		struct drm_hermes_kms_acquire_frame frame = {0};
		frame.flags = no_dmabuf ? 0 : (HERMES_KMS_FRAME_REQUEST_DMABUF | HERMES_KMS_FRAME_REQUEST_SYNC_FILE);
		if (ioctl(fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &frame) != 0) {
			perror("ACQUIRE_FRAME");
			return 1;
		}
		after_seq = frame.sequence;

		if (frame.width == 0) {
			printf("seq=%llu: no framebuffer yet\n", (unsigned long long)frame.sequence);
			continue;
		}

		uint32_t w = frame.width;
		uint32_t h = frame.height;

		if (!has_region) {
			rx = 0;
			ry = 0;
			rw = w;
			rh = h;
		}
		if (rx >= w || ry >= h) {
			fprintf(stderr, "region origin outside framebuffer (%ux%u)\n", w, h);
			return 1;
		}
		uint32_t cw = rw ? rw : w - rx;
		uint32_t ch = rh ? rh : h - ry;
		if (cw > w - rx || ch > h - ry) {
			fprintf(stderr, "region exceeds framebuffer (%ux%u)\n", w, h);
			return 1;
		}

		int dma_ok = (frame.flags & HERMES_KMS_FRAME_DMABUF_VALID) != 0;
		printf("[%s] seq=%llu fb=%u %ux%u fmt=0x%08x mod=0x%llx pitch=%u off=%u",
		       new_frame ? "new" : "poll", frame.sequence, frame.framebuffer_id, w, h, frame.format,
		       frame.modifier, frame.pitch[0], frame.offset[0]);
		if (frame.flags & HERMES_KMS_FRAME_DAMAGE_VALID)
			printf(" damage=(%u,%u,%u,%u)", frame.damage_x1, frame.damage_y1, frame.damage_x2, frame.damage_y2);
		printf(" dmabuf=%s\n", dma_ok ? "yes" : "NO");

		if (!dma_ok) {
			printf("  (no DMA-BUF%s)\n",
			       no_dmabuf ? ": metadata-only mode requested" : ": export failed");
			if (!no_dmabuf)
				return 1;
			count++;
			continue;
		}
		if (frame.plane_count != 1 || frame.dma_buf_fd[0] < 0 ||
		    frame.pitch[0] == 0) {
			fprintf(stderr, "unsupported DMA-BUF layout: planes=%u pitch=%u fd=%d\n",
				frame.plane_count, frame.pitch[0], frame.dma_buf_fd[0]);
			for (uint32_t i = 0; i < 4; i++)
				if (frame.dma_buf_fd[i] >= 0)
					close(frame.dma_buf_fd[i]);
			if (frame.sync_file_fd >= 0)
				close(frame.sync_file_fd);
			return 1;
		}

		/* wait for the producer fence (plane 0 sync file) */
		if (frame.sync_file_fd >= 0) {
			if (hermes_sync_file_wait(frame.sync_file_fd, 2000) < 0) {
				fprintf(stderr, "producer fence wait failed: %s\n",
					strerror(errno));
				close(frame.sync_file_fd);
				close(frame.dma_buf_fd[0]);
				return 1;
			}
			close(frame.sync_file_fd);
		}

		if ((size_t)h > (SIZE_MAX - frame.offset[0]) / frame.pitch[0]) {
			fprintf(stderr, "DMA-BUF mapping length overflow\n");
			close(frame.dma_buf_fd[0]);
			return 1;
		}
		size_t bpp = (frame.format == XR24 || frame.format == AR24) ? 4u : 0u;
		if (!bpp) {
			fprintf(stderr, "unsupported pixel format (expected XR24/AR24)\n");
			close(frame.dma_buf_fd[0]);
			return 1;
		}
		if ((size_t)w > SIZE_MAX / bpp ||
		    (size_t)frame.pitch[0] < (size_t)w * bpp) {
			fprintf(stderr, "DMA-BUF pitch is smaller than a pixel row\n");
			close(frame.dma_buf_fd[0]);
			return 1;
		}
		size_t map_len = frame.offset[0] + (size_t)frame.pitch[0] * h;
		off_t object_size = lseek(frame.dma_buf_fd[0], 0, SEEK_END);
		if (object_size < 0 || (uintmax_t)object_size < (uintmax_t)map_len) {
			fprintf(stderr, "frame metadata exceeds DMA-BUF: need=%zu size=%jd\n",
				map_len, (intmax_t)object_size);
			close(frame.dma_buf_fd[0]);
			return 1;
		}

		void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED,
		                 frame.dma_buf_fd[0], 0);
		if (map == MAP_FAILED) {
			perror("mmap dmabuf");
			close(frame.dma_buf_fd[0]);
			return 1;
		}

		struct dma_buf_sync dma_sync = {
			.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
		};
		if (ioctl(frame.dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &dma_sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC START");
			munmap(map, map_len);
			close(frame.dma_buf_fd[0]);
			return 1;
		}

		{
			const uint8_t *base = (const uint8_t *)map + frame.offset[0];
			uint32_t checksum = 0;
			for (uint32_t y = ry; y < ry + ch; y++) {
				const uint8_t *row = base + (size_t)y * frame.pitch[0] + (size_t)rx * bpp;
				for (uint32_t x = 0; x < cw; x++) {
					checksum = checksum * 31 + (uint32_t)row[x * bpp] +
					           ((uint32_t)row[x * bpp + 1] << 8) +
					           ((uint32_t)row[x * bpp + 2] << 16);
				}
			}
			printf("  region (%u,%u %ux%u) checksum=0x%08x%s", rx, ry, cw, ch, checksum,
			       (count && checksum != last_checksum) ? "  <-- CHANGED" : "");
			last_checksum = checksum;

			/* first 4 pixels of the region */
			for (uint32_t py = 0; py < 4 && py < ch; py++) {
				const uint8_t *row = base + (size_t)(ry + py) * frame.pitch[0] + (size_t)rx * bpp;
				printf("\n    row+%u: ", py);
				for (uint32_t px = 0; px < 4 && px < cw; px++)
					printf("%02x%02x%02x ", row[px * bpp], row[px * bpp + 1], row[px * bpp + 2]);
			}
			printf("\n");
		}

		dma_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		if (ioctl(frame.dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &dma_sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC END");
			munmap(map, map_len);
			close(frame.dma_buf_fd[0]);
			return 1;
		}
		munmap(map, map_len);
		close(frame.dma_buf_fd[0]);
		/* A timeout still produced a valid sample of the current scanout. */
		count++;
	}

	close(fd);
	return 0;
}
