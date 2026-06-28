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
// Build: cc -O2 -pthread -I/usr/include/libdrm -o hermes-export-stress \
//            hermes-export-stress.c -ldrm
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
static atomic_ullong g_acquires, g_acq_errors, g_flips;

static double now_s(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
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
	if (!res) return NULL;
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(g_fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			conn = c; break;
		}
		if (c) drmModeFreeConnector(c);
	}
	if (!conn) { drmModeFreeResources(res); return NULL; }
	drmModeModeInfo mode = conn->modes[0];
	uint32_t crtc = res->crtcs[0];

	uint32_t fb[2] = {0, 0}, handle[2] = {0, 0};
	for (int b = 0; b < 2; b++) {
		struct drm_mode_create_dumb cr = { .width = mode.hdisplay,
			.height = mode.vdisplay, .bpp = 32 };
		if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr)) return NULL;
		handle[b] = cr.handle;
		if (drmModeAddFB(g_fd, mode.hdisplay, mode.vdisplay, 24, 32,
				 cr.pitch, cr.handle, &fb[b])) return NULL;
	}
	drmModeSetCrtc(g_fd, crtc, fb[0], 0, 0, &conn->connector_id, 1, &mode);

	int cur = 0;
	while (!atomic_load(&g_stop)) {
		cur ^= 1;
		// Alternate page-flip and full SetCrtc: SetCrtc drops the fb /
		// rebuilds scanout state, exercising track_frame()'s cache drop.
		if (drmModePageFlip(g_fd, crtc, fb[cur], 0, NULL) == 0)
			atomic_fetch_add(&g_flips, 1);
		if ((atomic_load(&g_flips) & 0x3f) == 0)
			drmModeSetCrtc(g_fd, crtc, fb[cur], 0, 0,
				       &conn->connector_id, 1, &mode);
		usleep(2000);  // ~500 churns/sec
	}

	for (int b = 0; b < 2; b++) {
		if (fb[b]) drmModeRmFB(g_fd, fb[b]);
		struct drm_mode_destroy_dumb d = { .handle = handle[b] };
		if (handle[b]) drmIoctl(g_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	return NULL;
}

int main(int argc, char **argv) {
	int seconds = (argc > 1) ? atoi(argv[1]) : 10;
	int nthreads = (argc > 2) ? atoi(argv[2]) : 8;
	if (seconds < 1) seconds = 10;
	if (nthreads < 1) nthreads = 8;

	g_fd = open_hermes();
	if (g_fd < 0) { fprintf(stderr, "hermes-kms card not found\n"); return 2; }
	drmSetMaster(g_fd);

	pthread_t flip, cons[64];
	if (nthreads > 64) nthreads = 64;
	pthread_create(&flip, NULL, flip_thread, NULL);
	usleep(50000);  // let the first fb land
	for (int i = 0; i < nthreads; i++)
		pthread_create(&cons[i], NULL, acquire_thread, NULL);

	printf("stressing %ds with %d acquire threads + 1 producer...\n",
	       seconds, nthreads);
	double t0 = now_s();
	while (now_s() - t0 < seconds) usleep(200000);
	atomic_store(&g_stop, 1);

	for (int i = 0; i < nthreads; i++) pthread_join(cons[i], NULL);
	pthread_join(flip, NULL);

	unsigned long long acq = atomic_load(&g_acquires);
	unsigned long long err = atomic_load(&g_acq_errors);
	unsigned long long fl = atomic_load(&g_flips);
	printf("acquires=%llu errors=%llu flips=%llu\n", acq, err, fl);
	// Unexpected (non-ENODATA) ioctl errors are a failure.
	int fail = (err != 0) || (acq == 0);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
