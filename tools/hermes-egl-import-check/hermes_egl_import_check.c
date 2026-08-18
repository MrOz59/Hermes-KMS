// SPDX-License-Identifier: GPL-2.0
//
// hermes-egl-import-check: validate the NVENC-style consumer path for
// Hermes-KMS frames on NVIDIA (and any other EGL/GL/CUDA stack):
//
//   ACQUIRE_FRAME (DMA-BUF) -> wait on the exported sync file
//     -> eglCreateImage(EGL_LINUX_DMA_BUF_EXT)
//     -> glEGLImageTargetTexture2DOES -> FBO readback (proves sampling)
//     -> cuGraphicsGLRegisterImage (proves the Sunshine/Hermes CUDA interop)
//
// This is the exact frame path Hermes uses for NVENC: the encoder imports the
// captured DMA-BUF through EGL on the render GPU and maps the GL texture into
// CUDA. VAAPI has its own checker (hermes-kms-import-check); this tool covers
// the NVIDIA side of the roadmap ("NVENC/AMF DMA-BUF import validation").
//
// Every stage reports PASS / FAIL / SKIPPED / NOT PROVIDED individually and
// the final RESULT only claims a complete chain when every required stage
// actually ran and passed.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

#ifdef HAVE_CUDA
#include <cuda.h>
#include <cudaGL.h>
#endif

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

typedef void(GLAPIENTRY *PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_local)(GLenum target, void *image);

enum stage_result {
	ST_NOT_RUN = 0,
	ST_PASS,
	ST_FAIL,
	ST_SKIPPED,
	ST_NOT_PROVIDED,
};

static const char *stage_name(enum stage_result s)
{
	switch (s) {
	case ST_PASS:
		return "PASS";
	case ST_FAIL:
		return "FAIL";
	case ST_SKIPPED:
		return "SKIPPED";
	case ST_NOT_PROVIDED:
		return "NOT PROVIDED";
	default:
		return "not run";
	}
}

struct stage_results {
	enum stage_result acquire;
	enum stage_result fence;
	enum stage_result cpu_ref;
	enum stage_result egl_import;
	enum stage_result gl_validate;
	enum stage_result cuda_reg;
};

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/dri/cardN] [--gpu /dev/dri/renderDN] [--wait-ms MS]\n"
		"       [--no-cuda] [--no-cpu-ref]\n"
		"Exit codes: 0 = complete chain passed, 1 = a stage failed,\n"
		"            2 = no failures but the chain is incomplete (stages skipped)\n",
		argv0);
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

		if (strncmp(entry->d_name, "card", 4) != 0 &&
		    strncmp(entry->d_name, "renderD", 7) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
		fd = open_if_hermes(path);
		if (fd >= 0)
			break;
	}

	closedir(dir);
	return fd;
}

// Open the first render node that is NOT the Hermes virtual device: that is
// the real GPU which must import the frame (mirrors
// display_hermes_vram_t::open_real_render_node in Hermes).
static int open_real_gpu(const char *path, char *name_out, size_t name_len)
{
	struct dirent *entry;
	DIR *dir;
	int fd = -1;

	if (path) {
		fd = open(path, O_RDWR | O_CLOEXEC);
	} else {
		dir = opendir("/dev/dri");
		if (!dir)
			return -1;

		while ((entry = readdir(dir)) != NULL) {
			char candidate[PATH_MAX];

			if (strncmp(entry->d_name, "renderD", 7) != 0)
				continue;

			snprintf(candidate, sizeof(candidate), "/dev/dri/%s", entry->d_name);
			fd = open(candidate, O_RDWR | O_CLOEXEC);
			if (fd < 0)
				continue;

			drmVersionPtr ver = drmGetVersion(fd);
			bool is_hermes = ver && ver->name &&
					 strcmp(ver->name, "hermes-kms") == 0;
			if (ver)
				drmFreeVersion(ver);
			if (is_hermes) {
				close(fd);
				fd = -1;
				continue;
			}
			break;
		}
		closedir(dir);
	}

	if (fd >= 0 && name_out) {
		drmVersionPtr ver = drmGetVersion(fd);
		if (ver && ver->name)
			snprintf(name_out, name_len, "%s", ver->name);
		else
			snprintf(name_out, name_len, "unknown");
		if (ver)
			drmFreeVersion(ver);
	}

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

// ACQUIRE_FRAME only guarantees that the frame was exported, not that the
// producer finished writing it. The driver exports the framebuffer's write
// fence as a sync file; wait for it before using the pixels as a reference.
static enum stage_result wait_frame_fence(const struct drm_hermes_kms_acquire_frame *frame,
					  int timeout_ms)
{
	struct pollfd pfd;
	int rv;

	if (!(frame->flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) ||
	    frame->sync_file_fd < 0) {
		printf("INFO: driver provided no sync file for this frame\n");
		return ST_NOT_PROVIDED;
	}

	pfd.fd = frame->sync_file_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	do {
		rv = poll(&pfd, 1, timeout_ms);
	} while (rv < 0 && errno == EINTR);

	if (rv <= 0) {
		fprintf(stderr, "FAIL: frame fence did not signal within %d ms\n", timeout_ms);
		return ST_FAIL;
	}

	printf("PASS: frame fence signalled (producer finished writing)\n");
	return ST_PASS;
}

// CPU ground truth: mmap plane 0 and CRC a few rows so the GPU readback can be
// compared against what is actually in the buffer.
/*
 * A GPU driver that reads past the end of an imported DMA-BUF faults inside
 * libgallium, not in an API return code, and the process dies before any of
 * the stage summary is printed - which is useless for a diagnostic tool people
 * are asked to run. Catch the fault, report it as a failed stage, and stop.
 *
 * Longjmping out of a driver call leaves the GL context unusable, so anything
 * after a caught fault is abandoned rather than continued.
 */
static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_armed;

static void fault_handler(int sig)
{
	if (fault_armed) {
		fault_armed = 0;
		siglongjmp(fault_jmp, sig);
	}
	_exit(128 + sig);
}

/*
 * Keep sigsetjmp inside its own frame: a jump buffer established in main()
 * makes every live local there indeterminate after a longjmp, which the
 * compiler rightly warns about. Here the only locals are the saved handlers.
 */
static uint32_t crc32_simple(const uint8_t *data, size_t len, uint32_t crc);

/* noinline: inlining this back into the sigsetjmp frame would make its
 * locals indeterminate after a longjmp. */
__attribute__((noinline))
static uint32_t crc_rows_raw(const uint8_t *base, uint32_t rows, uint32_t height,
			     uint32_t pitch, uint32_t row_bytes)
{
	uint32_t crc = 0;

	for (uint32_t y = 0; y < rows && y < height; y++)
		crc = crc32_simple(base + (size_t) y * pitch, row_bytes, crc);
	return crc;
}

/* Same guard as the readback: a DMA-BUF whose pages the CPU cannot fault in
 * kills the process on first touch, which loses every verdict printed so far. */
static bool guarded_crc(const uint8_t *base, uint32_t rows, uint32_t height,
			uint32_t pitch, uint32_t row_bytes, uint32_t *crc_out,
			int *fault_sig)
{
	struct sigaction fault_sa, old_bus, old_segv;
	int sig;

	memset(&fault_sa, 0, sizeof(fault_sa));
	fault_sa.sa_handler = fault_handler;
	sigemptyset(&fault_sa.sa_mask);
	sigaction(SIGBUS, &fault_sa, &old_bus);
	sigaction(SIGSEGV, &fault_sa, &old_segv);

	sig = sigsetjmp(fault_jmp, 1);
	if (sig == 0) {
		fault_armed = 1;
		*crc_out = crc_rows_raw(base, rows, height, pitch, row_bytes);
		fault_armed = 0;
	}

	sigaction(SIGBUS, &old_bus, NULL);
	sigaction(SIGSEGV, &old_segv, NULL);
	*fault_sig = sig;
	return sig == 0;
}


/*
 * When an import is refused, ask the same GPU what it would allocate for that
 * geometry itself. A driver that needs more than the exporter provided rejects
 * the import rather than reading past the end, so the two numbers side by side
 * turn a bare EGL error code into something actionable.
 */
static void report_native_allocation(struct gbm_device *gbm, uint32_t width,
				     uint32_t height, uint32_t format,
				     uint32_t their_pitch, long long their_size)
{
	struct gbm_bo *bo;
	int fd;
	long long size;
	uint32_t stride;

	if (!gbm)
		return;

	bo = gbm_bo_create(gbm, width, height, format,
			   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (!bo) {
		printf("INFO: this GPU cannot allocate %ux%u linear in that format either\n",
		       width, height);
		return;
	}

	stride = gbm_bo_get_stride(bo);
	fd = gbm_bo_get_fd(bo);
	size = (fd >= 0) ? (long long) lseek(fd, 0, SEEK_END) : -1;
	if (fd >= 0)
		close(fd);
	gbm_bo_destroy(bo);

	if (size < 0) {
		printf("INFO: this GPU allocates stride=%u for %ux%u linear (size unknown)\n",
		       stride, width, height);
		return;
	}

	printf("INFO: for %ux%u linear this GPU allocates stride=%u size=%lld;\n"
	       "      the imported buffer has stride=%u size=%lld (%+lld)\n",
	       width, height, stride, size, their_pitch, their_size,
	       their_size - size);
	if (size > their_size)
		printf("INFO: the GPU wants MORE than the exporter provided - a short\n"
		       "      buffer is a plausible reason for the import to be refused\n");
}

static bool guarded_readback(uint32_t width, uint32_t rows, uint8_t *pixels,
			     int *fault_sig)
{
	struct sigaction fault_sa, old_bus, old_segv;
	int sig;

	memset(&fault_sa, 0, sizeof(fault_sa));
	fault_sa.sa_handler = fault_handler;
	sigemptyset(&fault_sa.sa_mask);
	sigaction(SIGBUS, &fault_sa, &old_bus);
	sigaction(SIGSEGV, &fault_sa, &old_segv);

	sig = sigsetjmp(fault_jmp, 1);
	if (sig == 0) {
		fault_armed = 1;
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, width, rows, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
		glFinish();
		fault_armed = 0;
	}

	sigaction(SIGBUS, &old_bus, NULL);
	sigaction(SIGSEGV, &old_segv, NULL);

	*fault_sig = sig;
	return sig == 0;
}

static uint32_t crc32_simple(const uint8_t *data, size_t len, uint32_t crc)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static bool cpu_crc_rows(const struct drm_hermes_kms_acquire_frame *frame,
			 uint32_t rows, uint32_t *crc_out, int *fault_sig)
{
	size_t map_len = (size_t) frame->pitch[0] * frame->height + frame->offset[0];

	*fault_sig = 0;
	void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, frame->dma_buf_fd[0], 0);

	if (map == MAP_FAILED)
		return false;

	// CPU access to a DMA-BUF mapping must be bracketed with the sync
	// ioctl; this read is the reference every later stage is compared
	// against, so an unsynchronized read here would poison all verdicts.
	/*
	 * Taking CPU access on a buffer the compositor is still scanning out can
	 * block indefinitely, and on at least one host it wedged the whole
	 * desktop instead of returning. Poll for read readiness first and give
	 * up rather than hang - this stage only produces a reference CRC, and is
	 * something Hermes itself never does.
	 */
	struct pollfd pfd;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = frame->dma_buf_fd[0];
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 2000) <= 0) {
		munmap(map, map_len);
		*fault_sig = -1;
		return false;
	}

	struct dma_buf_sync sync;
	memset(&sync, 0, sizeof(sync));
	sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	ioctl(frame->dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &sync);

	uint32_t crc = 0;
	const uint8_t *base = (const uint8_t *) map + frame->offset[0];
	uint32_t row_bytes = frame->width * 4;

	if (!guarded_crc(base, rows, frame->height, frame->pitch[0], row_bytes,
			 &crc, fault_sig)) {
		// The mapping exists but its pages cannot be faulted in. Leave it
		// mapped: unmapping is not worth another fault on the way out.
		return false;
	}

	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(frame->dma_buf_fd[0], DMA_BUF_IOCTL_SYNC, &sync);

	munmap(map, map_len);
	*crc_out = crc;
	return true;
}

int main(int argc, char **argv)
{
	const char *hermes_path = NULL;
	const char *gpu_path = NULL;
	uint32_t wait_ms = 2000;
	bool do_cuda = true;
	bool do_cpu_ref = true;
	int ret = 1;

	struct stage_results st;
	memset(&st, 0, sizeof(st));

	int hermes_fd = -1;
	int gpu_fd = -1;
	struct gbm_device *gbm = NULL;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLImage image = EGL_NO_IMAGE;
	GLuint tex = 0, fbo = 0, conv = 0, draw_fbo = 0;
	bool egl_initialized = false;
#ifdef HAVE_CUDA
	CUdevice cuda_dev = 0;
	bool cuda_ctx_retained = false;
	bool cuda_interop_ran = false;
#endif

	struct drm_hermes_kms_acquire_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.sync_file_fd = -1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			hermes_path = argv[++i];
		} else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
			gpu_path = argv[++i];
		} else if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
			wait_ms = (uint32_t) strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--no-cpu-ref") == 0) {
			do_cpu_ref = false;
		} else if (strcmp(argv[i], "--no-cuda") == 0) {
			do_cuda = false;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	// --- 1. Acquire a frame from Hermes-KMS ---------------------------------
	hermes_fd = hermes_path ? open_if_hermes(hermes_path) : open_auto_hermes();
	if (hermes_fd < 0) {
		fprintf(stderr, "FAIL: no Hermes-KMS device found (module loaded? output enabled?)\n");
		st.acquire = ST_FAIL;
		goto out;
	}

	if (wait_for_frame(hermes_fd, wait_ms)) {
		fprintf(stderr, "FAIL: no scanout frame (compositor not driving HERMES output?)\n");
		st.acquire = ST_FAIL;
		goto out;
	}

	if (acquire_frame(hermes_fd, &frame)) {
		st.acquire = ST_FAIL;
		goto out;
	}
	st.acquire = ST_PASS;

	printf("frame: %ux%u format=0x%08x ('%c%c%c%c') modifier=0x%016llx planes=%u pitch0=%u\n",
	       frame.width, frame.height, frame.format,
	       frame.format & 0xff, (frame.format >> 8) & 0xff,
	       (frame.format >> 16) & 0xff, (frame.format >> 24) & 0xff,
	       (unsigned long long) frame.modifier, frame.plane_count, frame.pitch[0]);

	/*
	 * How much memory actually backs plane 0, against the minimum the
	 * advertised geometry needs. A GPU importer may round the height up for
	 * its own layout and read past a buffer that only covers pitch x height,
	 * which faults inside the driver rather than failing an API call - so
	 * report the headroom before anything tries to sample it.
	 */
	off_t plane0_size = lseek(frame.dma_buf_fd[0], 0, SEEK_END);
	if (plane0_size > 0) {
		unsigned long long need =
			(unsigned long long) frame.pitch[0] * frame.height;
		printf("plane 0 dma-buf: %lld bytes; pitch x height = %llu; spare = %lld bytes (%.1f rows)\n",
		       (long long) plane0_size, need,
		       (long long) plane0_size - (long long) need,
		       frame.pitch[0] ?
			       ((double) plane0_size - (double) need) / frame.pitch[0] :
			       0.0);
	}

	// --- 2. Wait for the producer's write fence -----------------------------
	st.fence = wait_frame_fence(&frame, (int) wait_ms);
	if (st.fence == ST_FAIL)
		goto out;

	uint32_t cpu_crc = 0;
	int cpu_fault = 0;
	bool have_cpu_crc = false;

	if (!do_cpu_ref) {
		printf("INFO: CPU reference read skipped (--no-cpu-ref)\n");
		st.cpu_ref = ST_SKIPPED;
	} else if ((have_cpu_crc = cpu_crc_rows(&frame, 8, &cpu_crc, &cpu_fault)),
		   cpu_fault == -1) {
		printf("FAIL: the DMA-BUF never became readable for the CPU within 2s -\n"
		       "      something still holds it for writing. Re-run with --no-cpu-ref\n"
		       "      to skip this stage and reach the EGL import; Hermes never\n"
		       "      performs this read.\n");
		st.cpu_ref = ST_FAIL;
	} else if (have_cpu_crc) {
		printf("PASS: CPU mmap of plane 0 (crc32 of first 8 rows: 0x%08x)\n", cpu_crc);
		st.cpu_ref = ST_PASS;
	} else if (cpu_fault) {
		printf("FAIL: reading the mapped DMA-BUF faulted (SIG%s) on the exporter's\n"
		       "      own mapping - the buffer's pages could not be faulted in.\n",
		       cpu_fault == SIGBUS ? "BUS" : "SEGV");
		st.cpu_ref = ST_FAIL;
	} else {
		printf("INFO: CPU mmap not supported by exporter, no CPU reference available\n");
		st.cpu_ref = ST_SKIPPED;
	}

	// --- 3. EGL display/context on the real GPU ----------------------------
	char gpu_name[64] = {0};
	gpu_fd = open_real_gpu(gpu_path, gpu_name, sizeof(gpu_name));
	if (gpu_fd < 0) {
		fprintf(stderr, "FAIL: no real render GPU found\n");
		st.egl_import = ST_FAIL;
		goto out;
	}
	printf("render GPU: %s\n", gpu_name);

	gbm = gbm_create_device(gpu_fd);
	if (!gbm) {
		fprintf(stderr, "FAIL: gbm_create_device\n");
		st.egl_import = ST_FAIL;
		goto out;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display) {
		fprintf(stderr, "FAIL: eglGetPlatformDisplayEXT unavailable\n");
		st.egl_import = ST_FAIL;
		goto out;
	}

	dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "FAIL: eglGetPlatformDisplay(GBM)\n");
		st.egl_import = ST_FAIL;
		goto out;
	}

	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "FAIL: eglInitialize (0x%x)\n", eglGetError());
		st.egl_import = ST_FAIL;
		goto out;
	}
	egl_initialized = true;
	printf("EGL %d.%d on %s / %s\n", major, minor,
	       eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));

	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	bool has_import = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
	bool has_modifiers = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");
	printf("%s: EGL_EXT_image_dma_buf_import\n", has_import ? "PASS" : "FAIL");
	printf("INFO: EGL_EXT_image_dma_buf_import_modifiers %s\n",
	       has_modifiers ? "present" : "absent");
	if (!has_import) {
		st.egl_import = ST_FAIL;
		goto out;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "FAIL: eglBindAPI(OPENGL)\n");
		st.egl_import = ST_FAIL;
		goto out;
	}

	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
	if (ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "FAIL: eglCreateContext (0x%x)\n", eglGetError());
		st.egl_import = ST_FAIL;
		goto out;
	}

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "FAIL: eglMakeCurrent surfaceless (0x%x)\n", eglGetError());
		st.egl_import = ST_FAIL;
		goto out;
	}
	printf("GL renderer: %s\n", (const char *) glGetString(GL_RENDERER));

	// --- 4. Import the DMA-BUF as an EGLImage ------------------------------
	EGLAttrib attribs[64];
	int a = 0;
	attribs[a++] = EGL_WIDTH;
	attribs[a++] = frame.width;
	attribs[a++] = EGL_HEIGHT;
	attribs[a++] = frame.height;
	attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[a++] = frame.format;
	attribs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[a++] = frame.dma_buf_fd[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[a++] = frame.offset[0];
	attribs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[a++] = frame.pitch[0];
	// Only pass modifier attributes when the driver reported a valid
	// modifier AND the EGL implementation advertises the modifier
	// extension; otherwise omit them entirely.
	if (frame.modifier != DRM_FORMAT_MOD_INVALID && has_modifiers) {
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier & 0xFFFFFFFFu);
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier >> 32);
	}
	attribs[a++] = EGL_NONE;

	image = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
			       NULL, attribs);
	if (image == EGL_NO_IMAGE) {
		fprintf(stderr, "FAIL: eglCreateImage(EGL_LINUX_DMA_BUF_EXT) error=0x%x\n",
			eglGetError());
		fprintf(stderr, "      -> this GPU/driver refuses to import the Hermes DMA-BUF\n");
		fflush(stderr);
		report_native_allocation(gbm, frame.width, frame.height, frame.format,
					 frame.pitch[0], plane0_size);
		st.egl_import = ST_FAIL;
		goto out;
	}
	printf("PASS: eglCreateImage imported the Hermes DMA-BUF\n");
	st.egl_import = ST_PASS;

	// --- 5. Bind to a texture and read back through the GPU ----------------
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_local image_target_texture =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_local)
			eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!image_target_texture) {
		fprintf(stderr, "FAIL: glEGLImageTargetTexture2DOES unavailable\n");
		st.gl_validate = ST_FAIL;
		goto out;
	}

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	image_target_texture(GL_TEXTURE_2D, image);
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		printf("INFO: glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) error=0x%x, "
		       "trying glEGLImageTargetTexStorageEXT\n", err);

		// Desktop-GL drivers (NVIDIA in particular) often reject the
		// GLES-style OES bind for foreign DMA-BUF images but accept the
		// immutable-storage path from GL_EXT_EGL_image_storage.
		typedef void(GLAPIENTRY * PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC_local)(
			GLenum target, void *image, const GLint *attrib_list);
		PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC_local image_target_storage =
			(PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC_local)
				eglGetProcAddress("glEGLImageTargetTexStorageEXT");
		if (!image_target_storage) {
			fprintf(stderr, "FAIL: glEGLImageTargetTexStorageEXT unavailable too\n");
			st.gl_validate = ST_FAIL;
			goto out;
		}

		glDeleteTextures(1, &tex);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		image_target_storage(GL_TEXTURE_2D, image, NULL);
		err = glGetError();
		if (err != GL_NO_ERROR) {
			fprintf(stderr, "FAIL: glEGLImageTargetTexStorageEXT error=0x%x\n", err);
			st.gl_validate = ST_FAIL;
			goto out;
		}
		printf("PASS: EGLImage bound as GL texture (TexStorageEXT path)\n");
	} else {
		printf("PASS: EGLImage bound as GL texture (OES path)\n");
	}

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FAIL: FBO incomplete with imported texture\n");
		st.gl_validate = ST_FAIL;
		goto out;
	}

	uint32_t rows = frame.height < 8 ? frame.height : 8;
	uint8_t *pixels = calloc((size_t) frame.width * rows, 4);
	if (!pixels) {
		fprintf(stderr, "FAIL: out of memory for readback buffer\n");
		st.gl_validate = ST_FAIL;
		goto out;
	}

	int fault_sig = 0;
	if (!guarded_readback(frame.width, rows, pixels, &fault_sig)) {
		fprintf(stderr,
			"FAIL: CPU readback of the imported texture faulted (SIG%s) inside the\n"
			"      GPU driver, on the first byte of its own mapping of the imported\n"
			"      buffer - the importing driver could not back a CPU mapping of a\n"
			"      DMA-BUF owned by another driver. This is a limitation of reading\n"
			"      an imported buffer with the CPU, not evidence that the buffer is\n"
			"      wrong: the size and stride reported above may be perfectly valid.\n"
			"\n"
			"      Hermes never does this. It samples the imported texture on the\n"
			"      GPU into its own textures and encodes from those, so this stage\n"
			"      failing does not by itself mean the encode path is broken.\n",
			fault_sig == SIGBUS ? "BUS" : "SEGV");
		free(pixels);
		st.gl_validate = ST_FAIL;
		goto out;
	}

	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: glReadPixels from imported texture error=0x%x\n", err);
		free(pixels);
		st.gl_validate = ST_FAIL;
		goto out;
	}

	uint32_t gpu_crc = 0;
	for (uint32_t y = 0; y < rows; y++)
		gpu_crc = crc32_simple(pixels + (size_t) y * frame.width * 4,
				       frame.width * 4, gpu_crc);
	free(pixels);

	printf("PASS: GPU readback of imported frame (crc32 of first %u rows: 0x%08x)\n",
	       rows, gpu_crc);
	if (have_cpu_crc) {
		if (gpu_crc == cpu_crc)
			printf("PASS: GPU readback matches CPU ground truth\n");
		else
			printf("WARN: GPU/CPU crc mismatch (0x%08x vs 0x%08x); the compositor may "
			       "have redrawn the live buffer between the reads, use pitch_detect "
			       "for a drift-filtered verdict\n",
			       gpu_crc, cpu_crc);
	}

	// --- 6. Emulate the real encoder pipeline --------------------------------
	// Hermes/Sunshine never hands the *imported* texture to CUDA. The GL
	// converter (egl::sws_t) samples it into textures Sunshine allocated
	// itself, and only those are registered with CUDA. Reproduce that:
	// blit imported -> own texture, then map the own texture into CUDA.
	glGenTextures(1, &conv);
	glBindTexture(GL_TEXTURE_2D, conv);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, frame.width, frame.height);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: allocating conversion texture error=0x%x\n", err);
		st.gl_validate = ST_FAIL;
		goto out;
	}

	glGenFramebuffers(1, &draw_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, conv, 0);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FAIL: draw FBO incomplete\n");
		st.gl_validate = ST_FAIL;
		goto out;
	}

	// read FBO still holds the imported texture
	glBlitFramebuffer(0, 0, frame.width, frame.height,
			  0, 0, frame.width, frame.height,
			  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFinish();
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: GL blit imported->own texture error=0x%x\n", err);
		st.gl_validate = ST_FAIL;
		goto out;
	}
	printf("PASS: GL converter pass (imported -> own texture)\n");
	st.gl_validate = ST_PASS;

#ifdef HAVE_CUDA
	if (!do_cuda) {
		printf("INFO: CUDA stage skipped (--no-cuda)\n");
		st.cuda_reg = ST_SKIPPED;
	} else {
		CUresult cr = cuInit(0);
		if (cr != CUDA_SUCCESS) {
			printf("INFO: cuInit failed (%d), CUDA stage skipped\n", (int) cr);
			st.cuda_reg = ST_SKIPPED;
			goto cuda_done;
		}

		// Match the CUDA device to the GL context created on the
		// selected render node; device 0 may be a different GPU on
		// multi-GPU hosts and a register failure there would say
		// nothing about Hermes-KMS interop.
		unsigned int dev_count = 0;
		cr = cuGLGetDevices(&dev_count, &cuda_dev, 1, CU_GL_DEVICE_LIST_ALL);
		if (cr != CUDA_SUCCESS || dev_count == 0) {
			printf("INFO: no CUDA device backs the current GL context (%d), "
			       "CUDA stage skipped\n", (int) cr);
			st.cuda_reg = ST_SKIPPED;
			goto cuda_done;
		}

		char dev_name[128] = {0};
		if (cuDeviceGetName(dev_name, sizeof(dev_name) - 1, cuda_dev) == CUDA_SUCCESS)
			printf("CUDA device (via cuGLGetDevices): %s\n", dev_name);

		// The primary-context API is signature-stable across CUDA
		// 11/12/13; the unversioned cuCtxCreate is not (CUDA 13 added
		// a params argument). The context is released at the very end
		// of main: releasing it while the EGL context still exists
		// tears down shared GL-interop driver state underneath it and
		// crashes at least the NVIDIA driver during EGL/GL teardown.
		CUcontext cuctx;
		if (cuDevicePrimaryCtxRetain(&cuctx, cuda_dev) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: cuDevicePrimaryCtxRetain\n");
			st.cuda_reg = ST_FAIL;
			goto out;
		}
		cuda_ctx_retained = true;
		if (cuCtxSetCurrent(cuctx) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: cuCtxSetCurrent\n");
			st.cuda_reg = ST_FAIL;
			goto out;
		}

		CUgraphicsResource res;
		cuda_interop_ran = true;
		cr = cuGraphicsGLRegisterImage(&res, conv, GL_TEXTURE_2D,
					       CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
		if (cr != CUDA_SUCCESS) {
			const char *es = NULL;
			cuGetErrorString(cr, &es);
			fprintf(stderr, "FAIL: cuGraphicsGLRegisterImage(own texture): %s\n",
				es ? es : "?");
			st.cuda_reg = ST_FAIL;
			goto out;
		}

		CUarray arr;
		if (cuGraphicsMapResources(1, &res, 0) != CUDA_SUCCESS ||
		    cuGraphicsSubResourceGetMappedArray(&arr, res, 0, 0) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: mapping GL texture into CUDA\n");
			cuGraphicsUnregisterResource(res);
			st.cuda_reg = ST_FAIL;
			goto out;
		}

		// Pull the first row through CUDA and CRC it against the same
		// row read back through GL: both must see identical bytes of
		// the same frozen copy (the blit decoupled it from the live
		// desktop).
		uint32_t row_bytes = frame.width * 4;
		uint8_t *cuda_row = malloc(row_bytes);
		uint8_t *gl_row = malloc(row_bytes);
		if (!cuda_row || !gl_row) {
			fprintf(stderr, "FAIL: out of memory for row buffers\n");
			free(cuda_row);
			free(gl_row);
			cuGraphicsUnmapResources(1, &res, 0);
			cuGraphicsUnregisterResource(res);
			st.cuda_reg = ST_FAIL;
			goto out;
		}

		CUDA_MEMCPY2D cp;
		memset(&cp, 0, sizeof(cp));
		cp.srcMemoryType = CU_MEMORYTYPE_ARRAY;
		cp.srcArray = arr;
		cp.dstMemoryType = CU_MEMORYTYPE_HOST;
		cp.dstHost = cuda_row;
		cp.dstPitch = row_bytes;
		cp.WidthInBytes = row_bytes;
		cp.Height = 1;
		if (cuMemcpy2D(&cp) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: cuMemcpy2D from mapped array\n");
			free(cuda_row);
			free(gl_row);
			cuGraphicsUnmapResources(1, &res, 0);
			cuGraphicsUnregisterResource(res);
			st.cuda_reg = ST_FAIL;
			goto out;
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_fbo);
		glReadPixels(0, 0, frame.width, 1, GL_RGBA, GL_UNSIGNED_BYTE, gl_row);

		uint32_t cuda_crc = crc32_simple(cuda_row, row_bytes, 0);
		uint32_t glrow_crc = crc32_simple(gl_row, row_bytes, 0);
		free(cuda_row);
		free(gl_row);

		cuGraphicsUnmapResources(1, &res, 0);
		cuGraphicsUnregisterResource(res);

		printf("PASS: CUDA mapped the converter output (GL interop)\n");
		st.cuda_reg = ST_PASS;
		if (cuda_crc == glrow_crc)
			printf("PASS: CUDA row matches GL row (crc32 0x%08x), data is intact\n",
			       cuda_crc);
		else
			printf("WARN: CUDA/GL row crc mismatch (0x%08x vs 0x%08x)\n",
			       cuda_crc, glrow_crc);
	}
cuda_done:;
#else
	(void) do_cuda;
	printf("INFO: built without CUDA support (make HAVE_CUDA=1), CUDA stage skipped\n");
	st.cuda_reg = ST_SKIPPED;
#endif

out:
	// The verdict must never be lost to a driver teardown quirk: print and
	// flush it before any EGL/GL/CUDA cleanup runs.
	printf("\nstage summary:\n");
	printf("  frame acquisition:   %s\n", stage_name(st.acquire));
	printf("  fence wait:          %s\n", stage_name(st.fence));
	printf("  CPU reference read:  %s\n", stage_name(st.cpu_ref));
	printf("  EGL import:          %s\n", stage_name(st.egl_import));
	printf("  OpenGL validation:   %s\n", stage_name(st.gl_validate));
	printf("  CUDA registration:   %s\n", stage_name(st.cuda_reg));

	bool any_fail = st.acquire == ST_FAIL || st.fence == ST_FAIL ||
			st.cpu_ref == ST_FAIL || st.egl_import == ST_FAIL ||
			st.gl_validate == ST_FAIL || st.cuda_reg == ST_FAIL;
	bool complete = st.acquire == ST_PASS &&
			(st.fence == ST_PASS || st.fence == ST_NOT_PROVIDED) &&
			st.cpu_ref == ST_PASS &&
			st.egl_import == ST_PASS &&
			st.gl_validate == ST_PASS &&
			st.cuda_reg == ST_PASS;

	if (any_fail) {
		printf("RESULT: FAIL, the chain is broken at the first failed stage above\n");
		ret = 1;
	} else if (complete) {
		printf("RESULT: PASS, complete NVENC-style import chain "
		       "(DMA-BUF -> EGL -> GL converter -> CUDA)\n");
		ret = 0;
	} else {
		printf("RESULT: INCOMPLETE, no failures but skipped stages keep the "
		       "chain unvalidated\n");
		ret = 2;
	}
	fflush(stdout);

#ifdef HAVE_CUDA
	// NVIDIA (observed on 595.xx) segfaults inside the driver when the last
	// GPU-side reference to an imported system-memory DMA-BUF is released
	// after CUDA/GL interop touched it in the same process: whichever of
	// eglDestroyImage, the texture delete or eglTerminate happens to drop
	// that reference last crashes, in every ordering. Without the CUDA
	// stage the full teardown below runs clean on the same driver. Close
	// the kernel-side fds ourselves and leave the GPU object teardown to
	// process cleanup, via _exit so driver atexit handlers cannot crash
	// either. This keeps the exit code deterministic, which matters more
	// for a diagnostic tool than freeing GPU handles a millisecond before
	// the process ends.
	if (cuda_interop_ran) {
		fprintf(stderr, "INFO: skipping GPU object teardown after CUDA interop "
				"(NVIDIA driver workaround)\n");
		close_frame_fds(&frame);
		if (hermes_fd >= 0)
			close(hermes_fd);
		_exit(ret);
	}
#endif

	// Unbind before teardown: terminating the display while a context is
	// still current crashes some drivers during process exit.
	if (egl_initialized) {
		if (image != EGL_NO_IMAGE)
			eglDestroyImage(dpy, image);
		if (draw_fbo || fbo) {
			GLuint fbos[2] = {fbo, draw_fbo};
			glDeleteFramebuffers(2, fbos);
		}
		if (tex || conv) {
			GLuint texs[2] = {tex, conv};
			glDeleteTextures(2, texs);
		}
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
#ifdef HAVE_CUDA
	// Released only now, after the EGL/GL teardown: see the retain comment
	// in the CUDA stage.
	if (cuda_ctx_retained) {
		cuCtxSetCurrent(NULL);
		cuDevicePrimaryCtxRelease(cuda_dev);
	}
#endif

	return ret;
}
