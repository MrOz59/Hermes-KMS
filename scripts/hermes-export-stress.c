// hermes-export-stress: hammer ACQUIRE_FRAME (dma-buf + sync_file export) from
// many threads while a producer thread churns the scanout framebuffer, to shake
// out use-after-free / refcount imbalance in the export cache and fence
// lifetime. Designed to run under slub_debug (redzone/poison/UAF) or KASAN in
// the virtme-ng VM; clean output + clean dmesg == PASS.
//
// The export cache (export_obj[]/export_dmabuf[]) is keyed by a raw GEM object
// pointer and dropped from track_frame() when the fb changes, concurrently with
// consumers exporting that same fb. The producer here drives rapid modeset +
// page-flips between two dumb buffers so the cached object pointer is replaced
// and freed underneath in-flight ACQUIRE_FRAME calls.
//
/* Build: cc -O2 -pthread -I/usr/include/libdrm -o hermes-export-stress
 *            hermes-export-stress.c -ldrm
 */
// Run:   hermes-export-stress [seconds] [acquire-threads]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/hermes_kms_drm.h>

static int g_fd = -1;
static atomic_int g_stop;
static atomic_ullong g_acquires, g_acq_errors, g_flips, g_producer_errors;

static void producer_error(const char *operation) {
	int saved_errno = errno;

	atomic_fetch_add(&g_producer_errors, 1);
	atomic_store(&g_stop, 1);
	fprintf(stderr, "%s: %s\n", operation, strerror(saved_errno));
}

static double now_s(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int open_hermes(void) {
	for (int i = 0; i < 8; i++) {
		char p[64]; snprintf(p, sizeof(p), "/dev/dri/card%d", i);
		int f = open(p, O_RDWR | O_CLOEXEC);
		if (f < 0) continue;
		drmVersionPtr v = drmGetVersion(f);
		int ok = v && !strcmp(v->name, "hermes-kms");
		if (v) drmFreeVersion(v);
		if (ok) return f;
		close(f);
	}
	return -1;
}

// Consumer: tight ACQUIRE_FRAME(dma-buf + sync_file) + close-all loop.
static void *acquire_thread(void *arg) {
	(void)arg;
	while (!atomic_load(&g_stop)) {
		struct drm_hermes_kms_acquire_frame f;
		memset(&f, 0, sizeof(f));
		for (int i = 0; i < 4; i++) f.dma_buf_fd[i] = -1;
		f.sync_file_fd = -1;
		f.flags = HERMES_KMS_FRAME_REQUEST_DMABUF |
			  HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
		if (ioctl(g_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &f) < 0) {
			// ENODATA is expected when no fb is present this instant.
			if (errno != ENODATA) atomic_fetch_add(&g_acq_errors, 1);
			continue;
		}
		atomic_fetch_add(&g_acquires, 1);
		for (int i = 0; i < 4; i++)
			if (f.dma_buf_fd[i] >= 0) close(f.dma_buf_fd[i]);
		if (f.sync_file_fd >= 0) close(f.sync_file_fd);
	}
	return NULL;
}

// Producer: flip between two FBs so the scanout object pointer keeps changing,
// forcing the export cache to drop/replace entries under the consumers.
static void *flip_thread(void *arg) {
	(void)arg;
	drmModeRes *res = drmModeGetResources(g_fd);
	if (!res || res->count_crtcs < 1) {
		if (!res)
			producer_error("drmModeGetResources");
		else {
			errno = ENODEV;
			producer_error("no CRTC found");
		}
		if (res) drmModeFreeResources(res);
		atomic_store(&g_stop, 1);
		return NULL;
	}
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(g_fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			conn = c; break;
		}
		if (c) drmModeFreeConnector(c);
	}
	if (!conn) {
		errno = ENODEV;
		producer_error("no connected connector with a mode found");
		drmModeFreeResources(res);
		atomic_store(&g_stop, 1);
		return NULL;
	}
	drmModeModeInfo mode = conn->modes[0];
	uint32_t crtc = res->crtcs[0];

	uint32_t fb[2] = {0, 0}, handle[2] = {0, 0};
	int crtc_active = 0;
	for (int b = 0; b < 2; b++) {
		struct drm_mode_create_dumb cr = { .width = mode.hdisplay,
			.height = mode.vdisplay, .bpp = 32 };
		if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) {
			producer_error("DRM_IOCTL_MODE_CREATE_DUMB");
			goto out;
		}
		handle[b] = cr.handle;
		if (drmModeAddFB(g_fd, mode.hdisplay, mode.vdisplay, 24, 32,
				 cr.pitch, cr.handle, &fb[b])) {
			producer_error("drmModeAddFB");
			goto out;
		}
	}
	if (drmModeSetCrtc(g_fd, crtc, fb[0], 0, 0, &conn->connector_id, 1, &mode)) {
		producer_error("initial drmModeSetCrtc");
		goto out;
	}
	crtc_active = 1;

	int cur = 0;
	while (!atomic_load(&g_stop)) {
		cur ^= 1;
		// Alternate page-flip and full SetCrtc: SetCrtc drops the fb /
		// rebuilds scanout state, exercising track_frame()'s cache drop.
		if (drmModePageFlip(g_fd, crtc, fb[cur], 0, NULL) == 0) {
			atomic_fetch_add(&g_flips, 1);
		} else if (errno != EBUSY) {
			producer_error("drmModePageFlip");
			break;
		}
		if ((atomic_load(&g_flips) & 0x3f) == 0 &&
		    drmModeSetCrtc(g_fd, crtc, fb[cur], 0, 0,
				   &conn->connector_id, 1, &mode)) {
			producer_error("periodic drmModeSetCrtc");
			break;
		}
		usleep(2000);  // ~500 churns/sec
	}

out:
	if (crtc_active && drmModeSetCrtc(g_fd, crtc, 0, 0, 0, NULL, 0, NULL))
		producer_error("disable drmModeSetCrtc");
	for (int b = 0; b < 2; b++) {
		if (fb[b] && drmModeRmFB(g_fd, fb[b]))
			producer_error("drmModeRmFB");
		struct drm_mode_destroy_dumb d = { .handle = handle[b] };
		if (handle[b] &&
		    drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d))
			producer_error("DRM_IOCTL_MODE_DESTROY_DUMB");
	}
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return NULL;
}

static int parse_bounded_int(const char *text, int minimum, int maximum,
			     int *value)
{
	char *end = NULL;
	long parsed;

	if (!text || !*text || *text == '-')
		return -1;
	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || !end || *end || parsed < minimum || parsed > maximum)
		return -1;
	*value = (int)parsed;
	return 0;
}

int main(int argc, char **argv) {
	int seconds = 10;
	int nthreads = 8;

	if (argc > 3 ||
	    (argc > 1 && parse_bounded_int(argv[1], 1, 86400, &seconds)) ||
	    (argc > 2 && parse_bounded_int(argv[2], 1, 64, &nthreads))) {
		fprintf(stderr, "usage: %s [SECONDS(1..86400)] [THREADS(1..64)]\n",
			argv[0]);
		return 2;
	}

	g_fd = open_hermes();
	if (g_fd < 0) { fprintf(stderr, "hermes-kms card not found\n"); return 2; }
	struct drm_hermes_kms_set_output output = {
		.enabled = 1,
		.width = 1920,
		.height = 1080,
		.refresh_hz = 60,
	};
	if (ioctl(g_fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) != 0) {
		perror("SET_OUTPUT");
		close(g_fd);
		return 2;
	}
	if (drmSetMaster(g_fd) != 0) {
		perror("drmSetMaster");
		memset(&output, 0, sizeof(output));
		if (ioctl(g_fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) != 0)
			perror("disable SET_OUTPUT after drmSetMaster failure");
		close(g_fd);
		return 2;
	}

	pthread_t flip, cons[64];
	int consumer_count = 0;
	int flip_created = 0;
	int thread_errors = 0;
	int pthread_ret;

	pthread_ret = pthread_create(&flip, NULL, flip_thread, NULL);
	if (pthread_ret != 0) {
		fprintf(stderr, "pthread_create(producer): %s\n",
			strerror(pthread_ret));
		thread_errors++;
	} else {
		flip_created = 1;
		usleep(50000);  // let the first fb land
		for (int i = 0; i < nthreads && !atomic_load(&g_stop); i++) {
			pthread_ret = pthread_create(&cons[i], NULL,
						     acquire_thread, NULL);
			if (pthread_ret != 0) {
				fprintf(stderr, "pthread_create(consumer %d): %s\n", i,
					strerror(pthread_ret));
				thread_errors++;
				break;
			}
			consumer_count++;
		}
		if (consumer_count != nthreads)
			thread_errors++;
	}

	printf("stressing %ds with %d acquire threads + 1 producer...\n",
	       seconds, consumer_count);
	double t0 = now_s();
	while (!thread_errors && !atomic_load(&g_stop) &&
	       now_s() - t0 < seconds)
		usleep(200000);
	atomic_store(&g_stop, 1);

	for (int i = 0; i < consumer_count; i++) {
		pthread_ret = pthread_join(cons[i], NULL);
		if (pthread_ret != 0) {
			fprintf(stderr, "pthread_join(consumer %d): %s\n", i,
				strerror(pthread_ret));
			thread_errors++;
		}
	}
	if (flip_created) {
		pthread_ret = pthread_join(flip, NULL);
		if (pthread_ret != 0) {
			fprintf(stderr, "pthread_join(producer): %s\n",
				strerror(pthread_ret));
			thread_errors++;
		}
	}

	unsigned long long acq = atomic_load(&g_acquires);
	unsigned long long err = atomic_load(&g_acq_errors);
	unsigned long long fl = atomic_load(&g_flips);
	unsigned long long producer_err = atomic_load(&g_producer_errors);
	memset(&output, 0, sizeof(output));
	if (ioctl(g_fd, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output) != 0) {
		perror("disable SET_OUTPUT");
		producer_err++;
	}
	if (drmDropMaster(g_fd) != 0) {
		perror("drmDropMaster");
		producer_err++;
	}
	if (close(g_fd) != 0) {
		perror("close DRM device");
		producer_err++;
	}
	printf("acquires=%llu errors=%llu flips=%llu producer_errors=%llu thread_errors=%d\n",
	       acq, err, fl, producer_err, thread_errors);
	// A run that never flipped did not exercise the cache transition at all.
	int fail = (err != 0) || (acq == 0) || (fl == 0) ||
		   (producer_err != 0) || (thread_errors != 0);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
