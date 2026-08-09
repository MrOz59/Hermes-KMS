// SPDX-License-Identifier: GPL-2.0
// Detect which row pitch the GPU actually used when importing a Hermes-KMS
// DMA-BUF: the declared one (frame.pitch[0]) or a wrongly assumed width*4.
// Compares GPU-read rows against CPU ground truth under both hypotheses.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>

#include <linux/dma-buf.h>

#include <drm/drm_fourcc.h>
#include <drm/hermes_kms_drm.h>
#include <xf86drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/dri/cardN] [--gpu /dev/dri/renderDN]\n",
		argv0);
}

static int open_if_hermes(const char *path)
{
	struct drm_hermes_kms_version version;
	int fd = open(path, O_RDWR | O_CLOEXEC);
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
	DIR *dir = opendir("/dev/dri");
	int fd = -1;

	while (dir && (entry = readdir(dir))) {
		char path[PATH_MAX];
		if (strncmp(entry->d_name, "card", 4) && strncmp(entry->d_name, "renderD", 7))
			continue;
		snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
		fd = open_if_hermes(path);
		if (fd >= 0)
			break;
	}
	if (dir)
		closedir(dir);
	return fd;
}

// Open the render GPU that should import the frame: an explicit path when
// given, otherwise the first render node that is not the Hermes device.
static int open_real_gpu(const char *path)
{
	struct dirent *entry;
	DIR *dir;
	int fd = -1;

	if (path)
		return open(path, O_RDWR | O_CLOEXEC);

	dir = opendir("/dev/dri");
	while (dir && (entry = readdir(dir))) {
		char candidate[PATH_MAX];
		if (strncmp(entry->d_name, "renderD", 7))
			continue;
		snprintf(candidate, sizeof(candidate), "/dev/dri/%s", entry->d_name);
		fd = open(candidate, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr ver = drmGetVersion(fd);
		bool herm = ver && ver->name && !strcmp(ver->name, "hermes-kms");
		if (ver)
			drmFreeVersion(ver);
		if (herm) {
			close(fd);
			fd = -1;
			continue;
		}
		break;
	}
	if (dir)
		closedir(dir);
	return fd;
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

// The CPU snapshots below are the reference for every verdict; bracket them
// with the DMA-BUF sync ioctl as the mmap contract requires.
static void cpu_read_locked(int dmabuf_fd, uint8_t *dst, const uint8_t *src, size_t len)
{
	struct dma_buf_sync sync;

	memset(&sync, 0, sizeof(sync));
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);

	memcpy(dst, src, len);

	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int main(int argc, char **argv)
{
	const char *hermes_path = NULL;
	const char *gpu_path = NULL;
	int ret = 1;

	int hermes_fd = -1;
	int gpu_fd = -1;
	struct gbm_device *gbm = NULL;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLImage image = EGL_NO_IMAGE;
	GLuint tex = 0, fbo = 0;
	bool egl_initialized = false;
	uint8_t *cpu = MAP_FAILED;
	uint8_t *snap = NULL, *gpu_rows = NULL, *gpu_all = NULL;
	size_t map_len = 0;

	struct drm_hermes_kms_acquire_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.sync_file_fd = -1;

	setvbuf(stdout, NULL, _IONBF, 0);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			hermes_path = argv[++i];
		} else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
			gpu_path = argv[++i];
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	hermes_fd = hermes_path ? open_if_hermes(hermes_path) : open_auto_hermes();
	if (hermes_fd < 0) {
		fprintf(stderr, "no hermes device\n");
		goto out;
	}

	struct drm_hermes_kms_status status;
	memset(&status, 0, sizeof(status));
	if (ioctl(hermes_fd, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) < 0) {
		perror("GET_STATUS");
		goto out;
	}
	if (!(status.flags & HERMES_KMS_STATUS_FRAME_VALID)) {
		struct drm_hermes_kms_wait_frame wait;
		memset(&wait, 0, sizeof(wait));
		wait.after_sequence = status.frame_sequence;
		wait.timeout_ms = 10000;
		if (ioctl(hermes_fd, DRM_IOCTL_HERMES_KMS_WAIT_FRAME, &wait) < 0 ||
		    !(wait.flags & HERMES_KMS_WAIT_FRAME_READY)) {
			fprintf(stderr, "no scanout frame within 10s\n");
			goto out;
		}
	}

	frame.flags = HERMES_KMS_FRAME_REQUEST_DMABUF |
		      HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
	if (ioctl(hermes_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &frame) < 0 ||
	    !(frame.flags & HERMES_KMS_FRAME_DMABUF_VALID) ||
	    !frame.plane_count || frame.dma_buf_fd[0] < 0) {
		fprintf(stderr, "acquire failed\n");
		goto out;
	}

	// ACQUIRE_FRAME does not guarantee the producer finished writing; wait
	// for the exported write fence before freezing the CPU reference.
	if ((frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) && frame.sync_file_fd >= 0) {
		struct pollfd pfd = {.fd = frame.sync_file_fd, .events = POLLIN};
		int rv;
		do {
			rv = poll(&pfd, 1, 2000);
		} while (rv < 0 && errno == EINTR);
		if (rv <= 0) {
			fprintf(stderr, "frame fence did not signal within 2s\n");
			goto out;
		}
	}

	const uint32_t W = frame.width, H = frame.height;
	const uint32_t declared_pitch = frame.pitch[0];
	const uint32_t tight_pitch = W * 4;
	if (!W || !H || declared_pitch < tight_pitch) {
		fprintf(stderr, "implausible frame geometry %ux%u pitch=%u\n",
			W, H, declared_pitch);
		goto out;
	}
	printf("frame %ux%u declared_pitch=%u tight_pitch=%u\n", W, H, declared_pitch, tight_pitch);

	// CPU snapshot FIRST (freeze our reference before any GPU work)
	map_len = (size_t) declared_pitch * H + frame.offset[0];
	cpu = mmap(NULL, map_len, PROT_READ, MAP_SHARED, frame.dma_buf_fd[0], 0);
	if (cpu == MAP_FAILED) {
		fprintf(stderr, "mmap failed\n");
		goto out;
	}
	snap = malloc(map_len);
	if (!snap) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	cpu_read_locked(frame.dma_buf_fd[0], snap, cpu, map_len);

	// GPU import
	gpu_fd = open_real_gpu(gpu_path);
	if (gpu_fd < 0) {
		fprintf(stderr, "no render GPU found\n");
		goto out;
	}

	gbm = gbm_create_device(gpu_fd);
	if (!gbm) {
		fprintf(stderr, "gbm_create_device failed\n");
		goto out;
	}
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!gpd) {
		fprintf(stderr, "eglGetPlatformDisplayEXT unavailable\n");
		goto out;
	}
	dpy = gpd(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "EGL display init failed\n");
		dpy = EGL_NO_DISPLAY;
		goto out;
	}
	egl_initialized = true;
	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "eglBindAPI failed\n");
		goto out;
	}
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "EGL context setup failed (0x%x)\n", eglGetError());
		goto out;
	}

	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	bool has_modifiers = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");

	EGLAttrib attribs[32];
	int a = 0;
	attribs[a++] = EGL_WIDTH;
	attribs[a++] = W;
	attribs[a++] = EGL_HEIGHT;
	attribs[a++] = H;
	attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[a++] = frame.format;
	attribs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[a++] = frame.dma_buf_fd[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[a++] = frame.offset[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[a++] = declared_pitch;
	// Omit the modifier attributes when the driver reported none or the
	// EGL implementation does not advertise the modifier extension.
	if (frame.modifier != DRM_FORMAT_MOD_INVALID && has_modifiers) {
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier & 0xFFFFFFFFu);
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier >> 32);
	}
	attribs[a++] = EGL_NONE;

	image = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (image == EGL_NO_IMAGE) {
		fprintf(stderr, "import failed 0x%x\n", eglGetError());
		goto out;
	}

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	while (glGetError()) {}
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
	const char *bind_path = "OES";
	if (glGetError() != GL_NO_ERROR) {
		typedef void(GLAPIENTRY * TexStorageFn)(GLenum, void *, const GLint *);
		TexStorageFn tex_storage =
			(TexStorageFn) eglGetProcAddress("glEGLImageTargetTexStorageEXT");
		if (!tex_storage) {
			fprintf(stderr, "OES bind failed and glEGLImageTargetTexStorageEXT unavailable\n");
			goto out;
		}
		glDeleteTextures(1, &tex);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		tex_storage(GL_TEXTURE_2D, image, NULL);
		bind_path = "TexStorageEXT";
		if (glGetError() != GL_NO_ERROR) {
			fprintf(stderr, "both binds failed\n");
			goto out;
		}
	}
	printf("bind path: %s\n", bind_path);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FBO incomplete with imported texture\n");
		goto out;
	}

	gpu_rows = malloc((size_t) W * 4 * 32);
	if (!gpu_rows) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, W, 32, GL_BGRA, GL_UNSIGNED_BYTE, gpu_rows);
	glFinish();

	// Live desktop content can change a little between the CPU snapshot and
	// the GPU readback, so compare byte-match FRACTIONS: a pitch shear
	// misaligns everything, animation only touches small regions.
	const uint32_t rows_to_check = H < 200 ? H : 200;
	gpu_all = malloc((size_t) W * 4 * rows_to_check);
	if (!gpu_all) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	glReadPixels(0, 0, W, rows_to_check, GL_BGRA, GL_UNSIGNED_BYTE, gpu_all);
	glFinish();

	uint64_t eq_declared = 0, eq_tight = 0, total = 0;
	for (uint32_t y = 0; y < rows_to_check; y++) {
		const uint8_t *g = gpu_all + (size_t) y * W * 4;
		const uint8_t *c_decl = snap + frame.offset[0] + (size_t) y * declared_pitch;
		const uint8_t *c_tight = snap + frame.offset[0] + (size_t) y * tight_pitch;
		for (uint32_t i = 0; i < W * 4; i++) {
			eq_declared += (g[i] == c_decl[i]);
			eq_tight += (g[i] == c_tight[i]);
		}
		total += W * 4;
	}
	printf("byte match vs declared pitch (%u): %.2f%%\n", declared_pitch, 100.0 * eq_declared / total);
	printf("byte match vs tight pitch    (%u): %.2f%%\n", tight_pitch, 100.0 * eq_tight / total);

	// More hypotheses: vertical flip, R/B channel swap, both.
	uint64_t eq_flip = 0, eq_swap = 0, eq_flip_swap = 0;
	for (uint32_t y = 0; y < rows_to_check; y++) {
		const uint8_t *g = gpu_all + (size_t) y * W * 4;
		const uint8_t *c = snap + frame.offset[0] + (size_t) y * declared_pitch;
		const uint8_t *cf = snap + frame.offset[0] + (size_t) (H - 1 - y) * declared_pitch;
		for (uint32_t px = 0; px < W; px++) {
			const uint8_t *gp = g + px * 4;
			const uint8_t *cp = c + px * 4;
			const uint8_t *cfp = cf + px * 4;
			eq_flip += (gp[0] == cfp[0]) + (gp[1] == cfp[1]) + (gp[2] == cfp[2]) + (gp[3] == cfp[3]);
			eq_swap += (gp[0] == cp[2]) + (gp[1] == cp[1]) + (gp[2] == cp[0]) + (gp[3] == cp[3]);
			eq_flip_swap += (gp[0] == cfp[2]) + (gp[1] == cfp[1]) + (gp[2] == cfp[0]) + (gp[3] == cfp[3]);
		}
	}
	printf("byte match vs flipped rows:        %.2f%%\n", 100.0 * eq_flip / total);
	printf("byte match vs R/B swapped:         %.2f%%\n", 100.0 * eq_swap / total);
	printf("byte match vs flipped + swapped:   %.2f%%\n", 100.0 * eq_flip_swap / total);

	// Drift-proof comparison: KWin reuses the acquired buffer for a later
	// frame, so only rows whose CPU bytes are identical BEFORE and AFTER the
	// GPU readback are trustworthy references.
	{
		const uint32_t y0 = H > 600 ? 400 : H / 4;
		const uint32_t nrows = 200;
		uint8_t *gpu_mid = malloc((size_t) W * 4 * nrows);
		uint8_t *post = malloc(map_len);
		if (!gpu_mid || !post) {
			fprintf(stderr, "out of memory\n");
			free(gpu_mid);
			free(post);
			goto out;
		}
		glReadPixels(0, y0, W, nrows, GL_BGRA, GL_UNSIGNED_BYTE, gpu_mid);
		glFinish();
		cpu_read_locked(frame.dma_buf_fd[0], post, cpu, map_len);

		uint32_t stable = 0, st_decl = 0, st_tight = 0, st_other = 0;
		for (uint32_t r = 0; r < nrows && y0 + r < H; r++) {
			const size_t off_decl = frame.offset[0] + (size_t) (y0 + r) * declared_pitch;
			const size_t off_tight = frame.offset[0] + (size_t) (y0 + r) * tight_pitch;
			if (memcmp(snap + off_decl, post + off_decl, W * 4))
				continue;  // row changed while we were reading -> useless
			stable++;
			const uint8_t *g = gpu_mid + (size_t) r * W * 4;
			if (!memcmp(g, snap + off_decl, W * 4))
				st_decl++;
			else if (off_tight + W * 4 <= map_len && !memcmp(g, snap + off_tight, W * 4))
				st_tight++;
			else
				st_other++;
		}
		printf("drift-proof rows %u..%u: stable=%u  match_declared=%u  match_tight=%u  match_neither=%u\n",
		       y0, y0 + nrows, stable, st_decl, st_tight, st_other);
		if (stable && st_tight > st_decl)
			printf("VERDICT: GPU reads with width*4 pitch -> import shear confirmed\n");
		else if (stable && st_decl > st_tight)
			printf("VERDICT: GPU honors declared pitch on stable rows\n");
		free(gpu_mid);
		free(post);
	}

	// Brute force: which pitch does the GPU's view actually correspond to?
	// Sample busy rows mid-frame and scan candidate pitches.
	{
		const uint32_t y0 = H > 600 ? 500 : H / 2;
		const uint32_t nrows = 24;
		uint8_t *mid = malloc((size_t) W * 4 * nrows);
		if (!mid) {
			fprintf(stderr, "out of memory\n");
			goto out;
		}
		glReadPixels(0, y0, W, nrows, GL_BGRA, GL_UNSIGNED_BYTE, mid);
		glFinish();

		uint32_t best_pitch = 0;
		double best_frac = -1.0, second_frac = -1.0;
		uint32_t second_pitch = 0;
		for (uint32_t P = tight_pitch - 64; P <= tight_pitch + 5200; P += 4) {
			uint64_t eq = 0, tot = 0;
			for (uint32_t r = 0; r < nrows; r += 4) {
				const uint8_t *g = mid + (size_t) r * W * 4;
				size_t off = frame.offset[0] + (size_t) (y0 + r) * P;
				if (off + W * 4 > map_len)
					break;
				const uint8_t *c = snap + off;
				for (uint32_t i = 0; i < W * 4; i += 4) {  // sample every pixel's B channel + G
					eq += (g[i] == c[i]) + (g[i + 1] == c[i + 1]);
					tot += 2;
				}
			}
			double frac = tot ? (double) eq / tot : 0;
			if (frac > best_frac) {
				second_frac = best_frac;
				second_pitch = best_pitch;
				best_frac = frac;
				best_pitch = P;
			} else if (frac > second_frac) {
				second_frac = frac;
				second_pitch = P;
			}
		}
		printf("brute-force pitch scan (rows %u..%u):\n", y0, y0 + nrows);
		printf("  best pitch    = %u (%.2f%% match)%s\n", best_pitch, 100 * best_frac,
		       best_pitch == declared_pitch ? "  <-- declared" : (best_pitch == tight_pitch ? "  <-- width*4" : ""));
		printf("  second pitch  = %u (%.2f%% match)\n", second_pitch, 100 * second_frac);
		free(mid);
	}

	printf("cpu row0[0..15]: ");
	for (int i = 0; i < 16; i++)
		printf("%02x ", snap[frame.offset[0] + i]);
	printf("\ngpu row0[0..15]: ");
	for (int i = 0; i < 16; i++)
		printf("%02x ", gpu_all[i]);
	if (H > 100 && rows_to_check > 100) {
		printf("\ncpu row100[0..15]: ");
		for (int i = 0; i < 16; i++)
			printf("%02x ", snap[frame.offset[0] + (size_t) 100 * declared_pitch + i]);
		printf("\ngpu row100[0..15]: ");
		for (int i = 0; i < 16; i++)
			printf("%02x ", gpu_all[(size_t) 100 * W * 4 + i]);
	}
	printf("\n");

	ret = 0;

out:
	free(gpu_all);
	free(gpu_rows);
	free(snap);
	if (cpu != MAP_FAILED && map_len)
		munmap(cpu, map_len);
	if (egl_initialized) {
		if (fbo)
			glDeleteFramebuffers(1, &fbo);
		if (tex)
			glDeleteTextures(1, &tex);
		if (image != EGL_NO_IMAGE)
			eglDestroyImage(dpy, image);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctx);
		eglTerminate(dpy);
	}
	if (gbm)
		gbm_device_destroy(gbm);
	if (gpu_fd >= 0)
		close(gpu_fd);
	close_frame_fds(&frame);
	if (hermes_fd >= 0)
		close(hermes_fd);
	return ret;
}
