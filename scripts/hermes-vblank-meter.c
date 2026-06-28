// hermes-vblank-meter: measure the Hermes-KMS software vblank rate directly.
//
// Opens the hermes-kms primary node, sets the preferred mode on the connected
// connector (so the CRTC + vblank timer are active), then blocks on
// DRM_IOCTL_WAIT_VBLANK for N relative vblanks and reports the achieved Hz.
// This holds vblank enabled for the whole window (unlike a few page-flips), so
// the rate reflects the timer's steady-state pacing. Tool-independent and
// headless: works in the virtme-ng VM and on the host.
//
// Build: cc -O2 -I/usr/include/libdrm -o hermes-vblank-meter \
//            hermes-vblank-meter.c -ldrm
// Run:   hermes-vblank-meter [vblanks]   (default 120)
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	int target = (argc > 1) ? atoi(argv[1]) : 120;
	if (target < 1) target = 120;

	int fd = -1;
	for (int i = 0; i < 8 && fd < 0; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0) continue;
		drmVersionPtr v = drmGetVersion(f);
		if (v && strcmp(v->name, "hermes-kms") == 0) {
			fd = f;
		} else {
			close(f);
		}
		if (v) drmFreeVersion(v);
	}
	if (fd < 0) {
		fprintf(stderr, "hermes-kms card not found\n");
		return 2;
	}

	drmSetMaster(fd);
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) { fprintf(stderr, "no resources\n"); return 2; }

	// Find the connected connector + its preferred mode, and a usable CRTC.
	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		if (c) drmModeFreeConnector(c);
	}
	if (!conn) { fprintf(stderr, "no connected connector\n"); return 2; }

	drmModeModeInfo mode = conn->modes[0];  // preferred is first
	uint32_t crtc_id = res->crtcs[0];

	// Create a dumb buffer + framebuffer so SetCrtc succeeds.
	struct drm_mode_create_dumb creq = { .width = mode.hdisplay,
					     .height = mode.vdisplay, .bpp = 32 };
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
		perror("CREATE_DUMB"); return 2;
	}
	uint32_t fb_id = 0;
	if (drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32,
			 creq.pitch, creq.handle, &fb_id)) {
		perror("AddFB"); return 2;
	}
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0,
			   &conn->connector_id, 1, &mode)) {
		perror("SetCrtc"); return 2;
	}

	int vrefresh = mode.vrefresh ? mode.vrefresh : 60;
	printf("mode %dx%d@%d on connector %u crtc %u; waiting %d vblanks\n",
	       mode.hdisplay, mode.vdisplay, vrefresh,
	       conn->connector_id, crtc_id, target);

	// Prime one vblank, then time the next `target`.
	drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };
	if (drmWaitVBlank(fd, &vbl)) { perror("WAIT_VBLANK prime"); return 2; }

	double t0 = now_s();
	int missed = 0;
	unsigned int last = vbl.reply.sequence;
	for (int i = 0; i < target; i++) {
		drmVBlank w = { .request = { .type = DRM_VBLANK_RELATIVE,
					     .sequence = 1 } };
		if (drmWaitVBlank(fd, &w)) { perror("WAIT_VBLANK"); return 2; }
		// Each wait should advance the sequence by exactly 1; more means
		// the timer skipped vblanks (dropped flip slots).
		if (w.reply.sequence != last + 1)
			missed += (int)(w.reply.sequence - last - 1);
		last = w.reply.sequence;
	}
	double dt = now_s() - t0;

	double hz = target / dt;
	double dev = (hz - vrefresh) / vrefresh * 100.0;
	if (dev < 0) dev = -dev;
	printf("measured %.2f Hz over %.3fs (target %d, dev %.2f%%, missed %d)\n",
	       hz, dt, vrefresh, dev, missed);

	// Exit non-zero on >5%% deviation or any missed vblank.
	int fail = (dev > 5.0) || (missed != 0);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
