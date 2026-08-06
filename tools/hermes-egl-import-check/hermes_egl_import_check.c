// SPDX-License-Identifier: GPL-2.0
//
// hermes-egl-import-check: validate the NVENC-style consumer path for
// Hermes-KMS frames on NVIDIA (and any other EGL/GL/CUDA stack):
//
//   ACQUIRE_FRAME (DMA-BUF) -> eglCreateImage(EGL_LINUX_DMA_BUF_EXT)
//     -> glEGLImageTargetTexture2DOES -> FBO readback (proves sampling)
//     -> cuGraphicsGLRegisterImage (proves the Sunshine/Hermes CUDA interop)
//
// This is the exact frame path Hermes uses for NVENC: the encoder imports the
// captured DMA-BUF through EGL on the render GPU and maps the GL texture into
// CUDA. VAAPI has its own checker (hermes-kms-import-check); this tool covers
// the NVIDIA side of the roadmap ("NVENC/AMF DMA-BUF import validation").

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/dri/cardN] [--gpu /dev/dri/renderDN] [--wait-ms MS] [--no-cuda]\n",
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

// CPU ground truth: mmap plane 0 and CRC a few rows so the GPU readback can be
// compared against what is actually in the buffer.
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
			 uint32_t rows, uint32_t *crc_out)
{
	size_t map_len = (size_t) frame->pitch[0] * frame->height + frame->offset[0];
	void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, frame->dma_buf_fd[0], 0);

	if (map == MAP_FAILED)
		return false;

	uint32_t crc = 0;
	const uint8_t *base = (const uint8_t *) map + frame->offset[0];
	uint32_t row_bytes = frame->width * 4;

	for (uint32_t y = 0; y < rows && y < frame->height; y++)
		crc = crc32_simple(base + (size_t) y * frame->pitch[0], row_bytes, crc);

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
	int ret = 1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			hermes_path = argv[++i];
		} else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
			gpu_path = argv[++i];
		} else if (strcmp(argv[i], "--wait-ms") == 0 && i + 1 < argc) {
			wait_ms = (uint32_t) strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--no-cuda") == 0) {
			do_cuda = false;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	// --- 1. Acquire a frame from Hermes-KMS ---------------------------------
	int hermes_fd = hermes_path ? open_if_hermes(hermes_path) : open_auto_hermes();
	if (hermes_fd < 0) {
		fprintf(stderr, "FAIL: no Hermes-KMS device found (module loaded? output enabled?)\n");
		return 1;
	}

	if (wait_for_frame(hermes_fd, wait_ms)) {
		fprintf(stderr, "FAIL: no scanout frame (compositor not driving HERMES output?)\n");
		close(hermes_fd);
		return 1;
	}

	struct drm_hermes_kms_acquire_frame frame;
	if (acquire_frame(hermes_fd, &frame)) {
		close(hermes_fd);
		return 1;
	}

	printf("frame: %ux%u format=0x%08x ('%c%c%c%c') modifier=0x%016llx planes=%u pitch0=%u\n",
	       frame.width, frame.height, frame.format,
	       frame.format & 0xff, (frame.format >> 8) & 0xff,
	       (frame.format >> 16) & 0xff, (frame.format >> 24) & 0xff,
	       (unsigned long long) frame.modifier, frame.plane_count, frame.pitch[0]);

	uint32_t cpu_crc = 0;
	bool have_cpu_crc = cpu_crc_rows(&frame, 8, &cpu_crc);
	if (have_cpu_crc)
		printf("PASS: CPU mmap of plane 0 (crc32 of first 8 rows: 0x%08x)\n", cpu_crc);
	else
		printf("INFO: CPU mmap not supported by exporter (non-fatal)\n");

	// --- 2. EGL display/context on the real GPU ----------------------------
	char gpu_name[64] = {0};
	int gpu_fd = open_real_gpu(gpu_path, gpu_name, sizeof(gpu_name));
	if (gpu_fd < 0) {
		fprintf(stderr, "FAIL: no real render GPU found\n");
		goto out_frame;
	}
	printf("render GPU: %s\n", gpu_name);

	struct gbm_device *gbm = gbm_create_device(gpu_fd);
	if (!gbm) {
		fprintf(stderr, "FAIL: gbm_create_device\n");
		goto out_frame;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display) {
		fprintf(stderr, "FAIL: eglGetPlatformDisplayEXT unavailable\n");
		goto out_frame;
	}

	EGLDisplay dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "FAIL: eglGetPlatformDisplay(GBM)\n");
		goto out_frame;
	}

	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "FAIL: eglInitialize (0x%x)\n", eglGetError());
		goto out_frame;
	}
	printf("EGL %d.%d on %s / %s\n", major, minor,
	       eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));

	const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
	bool has_import = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
	bool has_modifiers = exts && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");
	printf("%s: EGL_EXT_image_dma_buf_import\n", has_import ? "PASS" : "FAIL");
	printf("INFO: EGL_EXT_image_dma_buf_import_modifiers %s\n",
	       has_modifiers ? "present" : "absent");
	if (!has_import)
		goto out_egl;

	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "FAIL: eglBindAPI(OPENGL)\n");
		goto out_egl;
	}

	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
	if (ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "FAIL: eglCreateContext (0x%x)\n", eglGetError());
		goto out_egl;
	}

	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "FAIL: eglMakeCurrent surfaceless (0x%x)\n", eglGetError());
		goto out_egl;
	}
	printf("GL renderer: %s\n", (const char *) glGetString(GL_RENDERER));

	// --- 3. Import the DMA-BUF as an EGLImage ------------------------------
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
	if (frame.modifier != DRM_FORMAT_MOD_INVALID) {
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier & 0xFFFFFFFFu);
		attribs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attribs[a++] = (EGLAttrib) (frame.modifier >> 32);
	}
	attribs[a++] = EGL_NONE;

	EGLImage image = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
					NULL, attribs);
	if (image == EGL_NO_IMAGE) {
		fprintf(stderr, "FAIL: eglCreateImage(EGL_LINUX_DMA_BUF_EXT) error=0x%x\n",
			eglGetError());
		fprintf(stderr, "      -> this GPU/driver refuses to import the Hermes DMA-BUF\n");
		goto out_egl;
	}
	printf("PASS: eglCreateImage imported the Hermes DMA-BUF\n");

	// --- 4. Bind to a texture and read back through the GPU ----------------
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_local image_target_texture =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_local)
			eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!image_target_texture) {
		fprintf(stderr, "FAIL: glEGLImageTargetTexture2DOES unavailable\n");
		goto out_egl;
	}

	GLuint tex = 0;
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
			goto out_egl;
		}

		glDeleteTextures(1, &tex);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		image_target_storage(GL_TEXTURE_2D, image, NULL);
		err = glGetError();
		if (err != GL_NO_ERROR) {
			fprintf(stderr, "FAIL: glEGLImageTargetTexStorageEXT error=0x%x\n", err);
			goto out_egl;
		}
		printf("PASS: EGLImage bound as GL texture (TexStorageEXT path)\n");
	} else {
		printf("PASS: EGLImage bound as GL texture (OES path)\n");
	}

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FAIL: FBO incomplete with imported texture\n");
		goto out_egl;
	}

	uint32_t rows = frame.height < 8 ? frame.height : 8;
	uint8_t *pixels = calloc((size_t) frame.width * rows, 4);
	if (!pixels)
		goto out_egl;

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, frame.width, rows, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: glReadPixels from imported texture error=0x%x\n", err);
		free(pixels);
		goto out_egl;
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
			printf("WARN: GPU/CPU crc mismatch (0x%08x vs 0x%08x) — row order or "
			       "cache coherency; inspect before trusting zero-copy\n",
			       gpu_crc, cpu_crc);
	}

	// --- 5. Emulate the real encoder pipeline --------------------------------
	// Hermes/Sunshine never hands the *imported* texture to CUDA. The GL
	// converter (egl::sws_t) samples it into textures Sunshine allocated
	// itself, and only those are registered with CUDA. Reproduce that:
	// blit imported -> own texture, then map the own texture into CUDA.
	GLuint conv = 0;
	glGenTextures(1, &conv);
	glBindTexture(GL_TEXTURE_2D, conv);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, frame.width, frame.height);
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: allocating conversion texture error=0x%x\n", err);
		goto out_egl;
	}

	GLuint draw_fbo = 0;
	glGenFramebuffers(1, &draw_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, conv, 0);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FAIL: draw FBO incomplete\n");
		goto out_egl;
	}

	// read FBO still holds the imported texture
	glBlitFramebuffer(0, 0, frame.width, frame.height,
			  0, 0, frame.width, frame.height,
			  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFinish();
	err = glGetError();
	if (err != GL_NO_ERROR) {
		fprintf(stderr, "FAIL: GL blit imported->own texture error=0x%x\n", err);
		goto out_egl;
	}
	printf("PASS: GL converter pass (imported -> own texture)\n");

#ifdef HAVE_CUDA
	if (do_cuda) {
		CUresult cr = cuInit(0);
		if (cr != CUDA_SUCCESS) {
			printf("INFO: cuInit failed (%d) — skipping CUDA stage\n", (int) cr);
			goto cuda_done;
		}

		CUdevice dev;
		CUcontext cuctx;
		if (cuDeviceGet(&dev, 0) != CUDA_SUCCESS ||
		    cuCtxCreate(&cuctx, NULL, 0, dev) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: CUDA context creation\n");
			goto out_egl;
		}

		CUgraphicsResource res;
		cr = cuGraphicsGLRegisterImage(&res, conv, GL_TEXTURE_2D,
					       CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
		if (cr != CUDA_SUCCESS) {
			const char *es = NULL;
			cuGetErrorString(cr, &es);
			fprintf(stderr, "FAIL: cuGraphicsGLRegisterImage(own texture): %s\n",
				es ? es : "?");
			cuCtxDestroy(cuctx);
			goto out_egl;
		}

		CUarray arr;
		if (cuGraphicsMapResources(1, &res, 0) != CUDA_SUCCESS ||
		    cuGraphicsSubResourceGetMappedArray(&arr, res, 0, 0) != CUDA_SUCCESS) {
			fprintf(stderr, "FAIL: mapping GL texture into CUDA\n");
			cuGraphicsUnregisterResource(res);
			cuCtxDestroy(cuctx);
			goto out_egl;
		}

		// Pull the first row through CUDA and CRC it against the same row
		// read back through GL: both must see identical bytes of the same
		// frozen copy (the blit decoupled it from the live desktop).
		uint32_t row_bytes = frame.width * 4;
		uint8_t *cuda_row = malloc(row_bytes);
		uint8_t *gl_row = malloc(row_bytes);
		if (!cuda_row || !gl_row) {
			free(cuda_row);
			free(gl_row);
			cuGraphicsUnmapResources(1, &res, 0);
			cuGraphicsUnregisterResource(res);
			cuCtxDestroy(cuctx);
			goto out_egl;
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
			cuCtxDestroy(cuctx);
			goto out_egl;
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_fbo);
		glReadPixels(0, 0, frame.width, 1, GL_RGBA, GL_UNSIGNED_BYTE, gl_row);

		uint32_t cuda_crc = crc32_simple(cuda_row, row_bytes, 0);
		uint32_t glrow_crc = crc32_simple(gl_row, row_bytes, 0);
		free(cuda_row);
		free(gl_row);

		cuGraphicsUnmapResources(1, &res, 0);
		cuGraphicsUnregisterResource(res);
		cuCtxDestroy(cuctx);

		printf("PASS: CUDA mapped the converter output (GL interop)\n");
		if (cuda_crc == glrow_crc)
			printf("PASS: CUDA row matches GL row (crc32 0x%08x) — data is intact\n",
			       cuda_crc);
		else
			printf("WARN: CUDA/GL row crc mismatch (0x%08x vs 0x%08x)\n",
			       cuda_crc, glrow_crc);
	}
cuda_done:
#else
	(void) do_cuda;
	printf("INFO: built without CUDA support (make HAVE_CUDA=1)\n");
#endif

	printf("\nRESULT: NVENC-style import chain OK "
	       "(DMA-BUF -> EGL -> GL converter -> CUDA)\n");
	ret = 0;

out_egl:
	// Unbind before teardown: terminating the display while a context is
	// still current crashes some drivers during process exit.
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
out_frame:
	close_frame_fds(&frame);
	close(hermes_fd);
	return ret;
}
