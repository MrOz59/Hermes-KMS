// SPDX-License-Identifier: GPL-2.0
// Detect which row pitch the GPU actually used when importing a Hermes-KMS
// DMA-BUF: the declared one (frame.pitch[0]) or a wrongly assumed width*4.
// Compares GPU-read rows against CPU ground truth under both hypotheses.

#include <errno.h>
#include <ctype.h>
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

#include "../hermes_session.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/dri/{cardN,renderDN}] --session-file PATH [--gpu /dev/dri/renderDN]\n",
		argv0);
}

static int open_if_hermes(const char *path)
{
	struct drm_hermes_kms_version version;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	int saved_errno;
	int ioctl_result;

	if (fd < 0)
		return -1;
	memset(&version, 0, sizeof(version));
	ioctl_result = hermes_session_require_driver(fd);
	if (ioctl_result == 0)
		ioctl_result = ioctl(fd, DRM_IOCTL_HERMES_KMS_GET_VERSION, &version);
	if (ioctl_result == 0 &&
	    strcmp(version.driver_name, "hermes-kms") == 0)
		return fd;
	saved_errno = ioctl_result < 0 ? errno : ENODEV;
	close(fd);
	errno = saved_errno;
	return -1;
}

static bool parse_node_index(const char *name, const char *prefix,
			     unsigned int *index)
{
	const char *suffix;
	char *end = NULL;
	unsigned long value;

	if (strncmp(name, prefix, strlen(prefix)) != 0)
		return false;
	suffix = name + strlen(prefix);
	if (!*suffix)
		return false;
	for (const char *p = suffix; *p; p++) {
		if (!isdigit((unsigned char)*p))
			return false;
	}
	errno = 0;
	value = strtoul(suffix, &end, 10);
	if (errno || !end || *end || value > UINT_MAX)
		return false;
	*index = (unsigned int)value;
	return true;
}

static int make_dri_path(char *path, size_t size, const char *name)
{
	int length = snprintf(path, size, "/dev/dri/%s", name);

	if (length < 0 || (size_t)length >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

// Open the render GPU that should import the frame: an explicit path when
// given, otherwise the first render node that is not the Hermes device.
static int open_if_real_gpu(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	drmVersionPtr version;
	bool valid;
	bool hermes;

	if (fd < 0)
		return -1;
	version = drmGetVersion(fd);
	valid = version && version->name;
	hermes = valid &&
		strcmp(version->name, "hermes-kms") == 0;
	if (version)
		drmFreeVersion(version);
	if (!valid || hermes) {
		close(fd);
		errno = hermes ? EINVAL : ENODEV;
		return -1;
	}
	return fd;
}

static int open_real_gpu(const char *path)
{
	struct dirent *entry;
	DIR *dir;
	char best_path[PATH_MAX] = {0};
	unsigned int best_index = UINT_MAX;

	if (path)
		return open_if_real_gpu(path);

	dir = opendir("/dev/dri");
	while (dir && (entry = readdir(dir))) {
		char candidate[PATH_MAX];
		unsigned int index;
		int fd;

		if (!parse_node_index(entry->d_name, "renderD", &index) ||
		    index >= best_index ||
		    make_dri_path(candidate, sizeof(candidate), entry->d_name) < 0)
			continue;
		fd = open_if_real_gpu(candidate);
		if (fd < 0)
			continue;
		close(fd);
		memcpy(best_path, candidate, strlen(candidate) + 1);
		best_index = index;
	}
	if (dir)
		closedir(dir);
	if (!best_path[0]) {
		errno = ENODEV;
		return -1;
	}
	return open_if_real_gpu(best_path);
}

static void close_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	for (uint32_t i = 0; i < 4U; i++) {
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
static int cpu_read_locked(int dmabuf_fd, uint8_t *dst, const uint8_t *src,
			   size_t len)
{
	struct dma_buf_sync sync;

	memset(&sync, 0, sizeof(sync));
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
		return -1;

	memcpy(dst, src, len);

	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	return ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static bool multiply_size(size_t left, size_t right, size_t *result)
{
	if (left && right > SIZE_MAX / left)
		return false;
	*result = left * right;
	return true;
}

static bool add_size(size_t left, size_t right, size_t *result)
{
	if (right > SIZE_MAX - left)
		return false;
	*result = left + right;
	return true;
}

static bool format_is_32bpp(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return true;
	default:
		return false;
	}
}

static bool read_pixels(uint32_t y, uint32_t width, uint32_t height,
			void *destination)
{
	GLenum error;

	glReadPixels(0, (GLint)y, (GLsizei)width, (GLsizei)height,
		     GL_BGRA, GL_UNSIGNED_BYTE, destination);
	glFinish();
	error = glGetError();
	if (error != GL_NO_ERROR) {
		fprintf(stderr, "glReadPixels failed (%#x)\n", (unsigned int)error);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	const char *hermes_path = NULL;
	const char *gpu_path = NULL;
	const char *session_file = NULL;
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
	uint8_t *snap = NULL, *gpu_all = NULL;
	size_t map_len = 0;

	struct drm_hermes_kms_acquire_frame frame;
	memset(&frame, 0, sizeof(frame));
	for (uint32_t i = 0; i < 4U; i++)
		frame.dma_buf_fd[i] = -1;
	frame.sync_file_fd = -1;

	setvbuf(stdout, NULL, _IONBF, 0);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			hermes_path = argv[++i];
		} else if (strcmp(argv[i], "--session-file") == 0 && i + 1 < argc) {
			session_file = argv[++i];
		} else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
			gpu_path = argv[++i];
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (!session_file) {
		fprintf(stderr, "--session-file is required for secure UAPI v11 capture\n");
		goto out;
	}
	if (hermes_path) {
		hermes_fd = open_if_hermes(hermes_path);
		if (hermes_fd >= 0 &&
		    hermes_session_bind_file(hermes_fd, session_file, NULL) < 0) {
			int saved_errno = errno;

			close(hermes_fd);
			hermes_fd = -1;
			errno = saved_errno;
		}
	} else {
		hermes_fd = hermes_session_open_bound_render(session_file, NULL, 0,
						     NULL);
	}
	if (hermes_fd < 0) {
		perror("open/bind Hermes session");
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
	if (ioctl(hermes_fd, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &frame) < 0) {
		perror("ACQUIRE_FRAME");
		goto out;
	}
	if (!(frame.flags & HERMES_KMS_FRAME_DMABUF_VALID) ||
	    frame.plane_count != 1U || frame.dma_buf_fd[0] < 0) {
		fprintf(stderr, "acquire returned no supported single-plane DMA-BUF\n");
		goto out;
	}

	// ACQUIRE_FRAME does not guarantee the producer finished writing; wait
	// for the exported write fence before freezing the CPU reference.
	if ((frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) && frame.sync_file_fd >= 0) {
		if (hermes_sync_file_wait(frame.sync_file_fd, 2000) < 0) {
			fprintf(stderr, "frame fence wait failed: %s\n", strerror(errno));
			goto out;
		}
	}

	const uint32_t W = frame.width, H = frame.height;
	const uint32_t declared_pitch = frame.pitch[0];
	uint32_t tight_pitch;
	size_t row_bytes;
	size_t plane_bytes;
	off_t object_size;

	if (!W || !H || W > (uint32_t)INT_MAX || H > (uint32_t)INT_MAX ||
	    W > UINT32_MAX / 4U || !format_is_32bpp(frame.format)) {
		fprintf(stderr, "unsupported frame geometry/format %ux%u fourcc=%#x\n",
			W, H, frame.format);
		goto out;
	}
	tight_pitch = W * 4U;
	row_bytes = (size_t)tight_pitch;
	if (declared_pitch < tight_pitch ||
	    !multiply_size((size_t)declared_pitch, (size_t)H, &plane_bytes) ||
	    !add_size((size_t)frame.offset[0], plane_bytes, &map_len)) {
		fprintf(stderr, "implausible frame geometry %ux%u pitch=%u\n",
			W, H, declared_pitch);
		goto out;
	}
	printf("frame %ux%u declared_pitch=%u tight_pitch=%u\n", W, H, declared_pitch, tight_pitch);

	// CPU snapshot FIRST (freeze our reference before any GPU work)
	object_size = lseek(frame.dma_buf_fd[0], 0, SEEK_END);
	if (object_size < 0 || (uintmax_t)object_size < (uintmax_t)map_len) {
		fprintf(stderr,
			"DMA-BUF is smaller than its declared layout (size=%jd need=%zu)\n",
			(intmax_t)object_size, map_len);
		goto out;
	}
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
	if (cpu_read_locked(frame.dma_buf_fd[0], snap, cpu, map_len) < 0) {
		perror("DMA_BUF_IOCTL_SYNC");
		goto out;
	}

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
		fprintf(stderr, "EGL context setup failed (%#x)\n",
			(unsigned int)eglGetError());
		goto out;
	}

	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	bool has_modifiers = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");

	EGLAttrib attribs[32];
	int a = 0;
	attribs[a++] = EGL_WIDTH;
	attribs[a++] = (EGLAttrib)W;
	attribs[a++] = EGL_HEIGHT;
	attribs[a++] = (EGLAttrib)H;
	attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[a++] = (EGLAttrib)frame.format;
	attribs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[a++] = (EGLAttrib)frame.dma_buf_fd[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[a++] = (EGLAttrib)frame.offset[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[a++] = (EGLAttrib)declared_pitch;
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
		fprintf(stderr, "import failed %#x\n", (unsigned int)eglGetError());
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

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "failed to configure packed GPU readback\n");
		goto out;
	}

	// Live desktop content can change a little between the CPU snapshot and
	// the GPU readback, so compare byte-match FRACTIONS: a pitch shear
	// misaligns everything, animation only touches small regions.
	const uint32_t rows_to_check = H < 200U ? H : 200U;
	size_t sample_bytes;
	if (!multiply_size(row_bytes, (size_t)rows_to_check, &sample_bytes)) {
		fprintf(stderr, "sample allocation size overflow\n");
		goto out;
	}
	gpu_all = malloc(sample_bytes);
	if (!gpu_all) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}
	if (!read_pixels(0, W, rows_to_check, gpu_all))
		goto out;

	uint64_t eq_declared = 0, eq_tight = 0, total = 0;
	for (uint32_t y = 0; y < rows_to_check; y++) {
		const uint8_t *g = gpu_all + (size_t)y * row_bytes;
		const uint8_t *c_decl = snap + frame.offset[0] + (size_t) y * declared_pitch;
		const uint8_t *c_tight = snap + frame.offset[0] + (size_t) y * tight_pitch;
		for (size_t i = 0; i < row_bytes; i++) {
			eq_declared += (uint64_t)(g[i] == c_decl[i]);
			eq_tight += (uint64_t)(g[i] == c_tight[i]);
		}
		total += (uint64_t)row_bytes;
	}
	printf("byte match vs declared pitch (%u): %.2f%%\n", declared_pitch,
	       100.0 * (double)eq_declared / (double)total);
	printf("byte match vs tight pitch    (%u): %.2f%%\n", tight_pitch,
	       100.0 * (double)eq_tight / (double)total);

	// More hypotheses: vertical flip, R/B channel swap, both.
	uint64_t eq_flip = 0, eq_swap = 0, eq_flip_swap = 0;
	for (uint32_t y = 0; y < rows_to_check; y++) {
		const uint8_t *g = gpu_all + (size_t)y * row_bytes;
		const uint8_t *c = snap + frame.offset[0] + (size_t) y * declared_pitch;
		const uint8_t *cf = snap + frame.offset[0] + (size_t) (H - 1 - y) * declared_pitch;
		for (uint32_t px = 0; px < W; px++) {
			const size_t pixel_offset = (size_t)px * 4U;
			const uint8_t *gp = g + pixel_offset;
			const uint8_t *cp = c + pixel_offset;
			const uint8_t *cfp = cf + pixel_offset;
			eq_flip += (uint64_t)(gp[0] == cfp[0]);
			eq_flip += (uint64_t)(gp[1] == cfp[1]);
			eq_flip += (uint64_t)(gp[2] == cfp[2]);
			eq_flip += (uint64_t)(gp[3] == cfp[3]);
			eq_swap += (uint64_t)(gp[0] == cp[2]);
			eq_swap += (uint64_t)(gp[1] == cp[1]);
			eq_swap += (uint64_t)(gp[2] == cp[0]);
			eq_swap += (uint64_t)(gp[3] == cp[3]);
			eq_flip_swap += (uint64_t)(gp[0] == cfp[2]);
			eq_flip_swap += (uint64_t)(gp[1] == cfp[1]);
			eq_flip_swap += (uint64_t)(gp[2] == cfp[0]);
			eq_flip_swap += (uint64_t)(gp[3] == cfp[3]);
		}
	}
	printf("byte match vs flipped rows:        %.2f%%\n",
	       100.0 * (double)eq_flip / (double)total);
	printf("byte match vs R/B swapped:         %.2f%%\n",
	       100.0 * (double)eq_swap / (double)total);
	printf("byte match vs flipped + swapped:   %.2f%%\n",
	       100.0 * (double)eq_flip_swap / (double)total);

	// Drift-proof comparison: KWin reuses the acquired buffer for a later
	// frame, so only rows whose CPU bytes are identical BEFORE and AFTER the
	// GPU readback are trustworthy references.
	{
		const uint32_t y0 = H > 600U ? 400U : H / 4U;
		const uint32_t remaining_rows = H - y0;
		const uint32_t nrows = remaining_rows < 200U ? remaining_rows : 200U;
		size_t middle_bytes;
		if (!multiply_size(row_bytes, (size_t)nrows, &middle_bytes)) {
			fprintf(stderr, "middle sample allocation size overflow\n");
			goto out;
		}
		uint8_t *gpu_mid = malloc(middle_bytes);
		uint8_t *post = malloc(map_len);
		if (!gpu_mid || !post) {
			fprintf(stderr, "out of memory\n");
			free(gpu_mid);
			free(post);
			goto out;
		}
		if (!read_pixels(y0, W, nrows, gpu_mid)) {
			free(gpu_mid);
			free(post);
			goto out;
		}
		if (cpu_read_locked(frame.dma_buf_fd[0], post, cpu, map_len) < 0) {
			perror("DMA_BUF_IOCTL_SYNC");
			free(gpu_mid);
			free(post);
			goto out;
		}

		uint32_t stable = 0, st_decl = 0, st_tight = 0, st_other = 0;
		for (uint32_t r = 0; r < nrows; r++) {
			const size_t off_decl = frame.offset[0] + (size_t) (y0 + r) * declared_pitch;
			const size_t off_tight = frame.offset[0] + (size_t) (y0 + r) * tight_pitch;
			if (memcmp(snap + off_decl, post + off_decl, row_bytes))
				continue;  // row changed while we were reading -> useless
			stable++;
			const uint8_t *g = gpu_mid + (size_t)r * row_bytes;
			if (!memcmp(g, snap + off_decl, row_bytes))
				st_decl++;
			else if (off_tight <= map_len - row_bytes &&
				 !memcmp(g, snap + off_tight, row_bytes))
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
		const uint32_t y0 = H > 600U ? 500U : H / 2U;
		const uint32_t remaining_rows = H - y0;
		const uint32_t nrows = remaining_rows < 24U ? remaining_rows : 24U;
		size_t middle_bytes;
		if (!multiply_size(row_bytes, (size_t)nrows, &middle_bytes)) {
			fprintf(stderr, "pitch sample allocation size overflow\n");
			goto out;
		}
		uint8_t *mid = malloc(middle_bytes);
		if (!mid) {
			fprintf(stderr, "out of memory\n");
			goto out;
		}
		if (!read_pixels(y0, W, nrows, mid)) {
			free(mid);
			goto out;
		}

		uint32_t best_pitch = 0;
		double best_frac = -1.0, second_frac = -1.0;
		uint32_t second_pitch = 0;
		uint64_t scan_start = tight_pitch > 64U ? (uint64_t)tight_pitch - 64U : 4U;
		const uint64_t scan_end_unclamped = (uint64_t)tight_pitch + 5200U;
		const uint64_t scan_end = scan_end_unclamped < UINT32_MAX ?
			scan_end_unclamped : UINT32_MAX;
		scan_start = (scan_start + 3U) & ~UINT64_C(3);
		for (uint64_t candidate = scan_start; candidate <= scan_end;
		     candidate += 4U) {
			const uint32_t P = (uint32_t)candidate;
			uint64_t eq = 0, tot = 0;
			for (uint32_t r = 0; r < nrows; r += 4) {
				const uint8_t *g = mid + (size_t)r * row_bytes;
				size_t candidate_offset;
				size_t off;
				if (!multiply_size((size_t)(y0 + r), (size_t)P,
						   &candidate_offset) ||
				    !add_size((size_t)frame.offset[0], candidate_offset,
					      &off) || off > map_len - row_bytes)
					break;
				const uint8_t *c = snap + off;
				for (size_t i = 0; i < row_bytes; i += 4U) {
					eq += (uint64_t)(g[i] == c[i]);
					eq += (uint64_t)(g[i + 1U] == c[i + 1U]);
					tot += 2U;
				}
			}
			double frac = tot ? (double)eq / (double)tot : 0.0;
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
		printf("  best pitch    = %u (%.2f%% match)%s\n", best_pitch, 100.0 * best_frac,
		       best_pitch == declared_pitch ? "  <-- declared" : (best_pitch == tight_pitch ? "  <-- width*4" : ""));
		printf("  second pitch  = %u (%.2f%% match)\n", second_pitch, 100.0 * second_frac);
		free(mid);
	}

	const size_t preview_bytes = row_bytes < 16U ? row_bytes : 16U;
	printf("cpu row0[0..%zu]: ", preview_bytes - 1U);
	for (size_t i = 0; i < preview_bytes; i++)
		printf("%02x ", (unsigned int)snap[(size_t)frame.offset[0] + i]);
	printf("\ngpu row0[0..%zu]: ", preview_bytes - 1U);
	for (size_t i = 0; i < preview_bytes; i++)
		printf("%02x ", (unsigned int)gpu_all[i]);
	if (H > 100 && rows_to_check > 100) {
		printf("\ncpu row100[0..%zu]: ", preview_bytes - 1U);
		for (size_t i = 0; i < preview_bytes; i++)
			printf("%02x ", (unsigned int)snap[frame.offset[0] +
			       (size_t)100U * declared_pitch + i]);
		printf("\ngpu row100[0..%zu]: ", preview_bytes - 1U);
		for (size_t i = 0; i < preview_bytes; i++)
			printf("%02x ", (unsigned int)gpu_all[(size_t)100U * row_bytes + i]);
	}
	printf("\n");

	ret = 0;

out:
	free(gpu_all);
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
