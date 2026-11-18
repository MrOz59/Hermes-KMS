// SPDX-License-Identifier: GPL-2.0
//
// hermes-import-cache-check: does an EGLImage of a DMA-BUF that ANOTHER
// process allocated keep showing what that process renders into the buffer
// later, or only what it held when the image was first sampled?
//
// Capture consumers such as Hermes keep one import per scanout buffer instead
// of importing every frame (fewer eglCreateImage calls, and on NVIDIA a bound
// on the VRAM that sampled cross-process imports leave behind). That is only
// correct if the driver samples the live buffer. This tool measures exactly
// that, without Hermes-KMS or a compositor:
//
//   child  (producer): allocates N modified GBM BOs on --gpu, sends the
//          DMA-BUFs over a socket, and renders a known pattern with a new
//          seed into one buffer per request, finishing (glFinish) before it
//          acknowledges;
//   parent (consumer): imports each buffer once and keeps the import, then
//          after every render checks the whole cached image against the new
//          seed, and optionally against a fresh import of the same buffer.
//
// A cached image that still matches the previous seed is STALE; one matching
// neither is CORRUPT. Writes are completed before sampling, so this tests
// import coherence, not fence handling or a live compositor's scheduling.

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "../hermes-gpu-scanout-test/modifiers.h"
#include "../hermes_nvidia_version.h"
#include "../hermes-gpu-scanout-test/pattern.h"

#define FORMAT DRM_FORMAT_ARGB8888
#define MAX_BUFFERS 8u
#define MAX_PLANES 4u
#define MAX_SAMPLES 4096u

enum { OP_RENDER = 1, OP_QUIT = 2 };

struct hello {
	int32_t status; /* 0 = ready, otherwise the producer failed */
	uint32_t buffers, width, height, planes;
	uint64_t modifier;
	uint32_t pitch[MAX_BUFFERS][MAX_PLANES];
	uint32_t offset[MAX_BUFFERS][MAX_PLANES];
};

struct command {
	uint32_t op, index, seed;
};

struct options {
	const char *gpu_path;
	uint32_t buffers, rounds, width, height, churn;
	bool linear, allow_other, fresh;
};

struct egl {
	struct gbm_device *gbm;
	EGLDisplay display;
	EGLContext context;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
};

struct import {
	EGLImageKHR image;
	GLuint tex, fbo;
};

static int has_extension(const char *list, const char *name)
{
	size_t n = strlen(name);
	const char *p = list;

	while (p && (p = strstr(p, name))) {
		if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' '))
			return 1;
		p += n;
	}
	return 0;
}

static int number(const char *s, uint32_t *v, uint32_t min, uint32_t max)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(s, &end, 10);
	if (!*s || *end || errno || s[0] == '-' || n < min || n > max)
		return -1;
	*v = (uint32_t)n;
	return 0;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static double median(double *v, size_t n)
{
	if (!n)
		return 0.0;
	qsort(v, n, sizeof(*v), compare_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static void usage(FILE *f, const char *name)
{
	fprintf(f, "Usage: %s --gpu /dev/dri/renderDN [options]\n"
		"  --buffers N        producer buffers kept imported, 1..%u (default 3)\n"
		"  --rounds N         renders per buffer, 2..10000 (default 40)\n"
		"  --width N --height N  default 2560x1440 (beyond the 2 MiB boundary)\n"
		"  --modifiers SRC    egl (default: any renderable layout) or linear\n"
		"  --no-fresh         skip the fresh-import comparison\n"
		"  --churn N          afterwards, import/sample/destroy one buffer N times\n"
		"                     (the per-frame pattern); watch the GPU's memory use\n"
		"  --allow-other-gpu  permit a non-NVIDIA hardware control run\n"
		"Exit 0: every cached re-sample showed the producer's latest render.\n"
		"Exit 1: stale/corrupt samples or a setup failure. Exit 2: invalid arguments.\n",
		name, MAX_BUFFERS);
}

static int init_egl(struct egl *e, int fd, bool allow_other, bool quiet)
{
	drmVersionPtr version = drmGetVersion(fd);
	bool nvidia = version && version->name && !strcmp(version->name, "nvidia-drm");

	if (!quiet)
		printf("GPU DRM driver: %s\n", version && version->name ? version->name : "unknown");
	if (version)
		drmFreeVersion(version);
	if (!nvidia && !allow_other) {
		fprintf(stderr, "FAIL: GPU is not nvidia-drm (use --allow-other-gpu for a control run)\n");
		return -1;
	}
	e->gbm = gbm_create_device(fd);
	if (!e->gbm)
		return -1;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display)
		return -1;
	e->display = get_display(EGL_PLATFORM_GBM_KHR, e->gbm, NULL);
	if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, NULL, NULL))
		return -1;
	const char *extensions = eglQueryString(e->display, EGL_EXTENSIONS);
	if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
	    !has_extension(extensions, "EGL_KHR_no_config_context") ||
	    !has_extension(extensions, "EGL_KHR_surfaceless_context")) {
		fprintf(stderr, "FAIL: EGL lacks dma-buf modifiers, no-config or surfaceless contexts\n");
		return -1;
	}
	e->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	e->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	if (!e->create_image || !e->destroy_image || !eglBindAPI(EGL_OPENGL_API))
		return -1;
	const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	e->context = eglCreateContext(e->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (e->context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, e->context))
		return -1;
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	if (!quiet)
		printf("GL vendor: %s; renderer: %s\n", vendor ? vendor : "unknown",
		       renderer ? renderer : "unknown");
	if (!vendor || !renderer || (!allow_other && !strstr(vendor, "NVIDIA")) ||
	    strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) {
		fprintf(stderr, "FAIL: not a hardware%s GL renderer\n", allow_other ? "" : " NVIDIA");
		return -1;
	}
	return 0;
}

static int cleanup_egl(struct egl *e)
{
	int error = 0;

	if (e->display != EGL_NO_DISPLAY) {
		if (!eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
			error = 1;
		if (e->context != EGL_NO_CONTEXT && !eglDestroyContext(e->display, e->context))
			error = 1;
		if (!eglTerminate(e->display))
			error = 1;
	}
	if (e->gbm)
		gbm_device_destroy(e->gbm);
	return error ? -1 : 0;
}

/* Import, bind to a texture (OES, then TexStorageEXT) and attach to an FBO. */
static int import_buffer(struct egl *e, struct import *out, uint32_t w, uint32_t h,
			 const int *fds, const uint32_t *pitch, const uint32_t *offset,
			 uint32_t planes, uint64_t modifier)
{
	static const EGLint keys[MAX_PLANES][5] = {
		{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		 EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		 EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		 EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		 EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
	};
	EGLint a[48] = {EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h, EGL_LINUX_DRM_FOURCC_EXT, FORMAT};
	size_t n = 6;

	memset(out, 0, sizeof(*out));
	out->image = EGL_NO_IMAGE_KHR;
	for (uint32_t i = 0; i < planes; i++) {
		if (pitch[i] > INT_MAX || offset[i] > INT_MAX)
			return -1;
		const EGLint values[5] = {fds[i], (EGLint)offset[i], (EGLint)pitch[i],
			(EGLint)(uint32_t)modifier, (EGLint)(uint32_t)(modifier >> 32)};
		for (unsigned int k = 0; k < 5; k++) {
			a[n++] = keys[i][k];
			a[n++] = values[k];
		}
	}
	a[n] = EGL_NONE;
	out->image = e->create_image(e->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
	if (out->image == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "FAIL: eglCreateImage: 0x%x\n", eglGetError());
		return -1;
	}
	typedef void (GLAPIENTRY *bind_oes_t)(GLenum, void *);
	typedef void (GLAPIENTRY *bind_storage_t)(GLenum, void *, const GLint *);
	bind_oes_t bind_oes = (bind_oes_t)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	bind_storage_t bind_storage = (bind_storage_t)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
	while (glGetError() != GL_NO_ERROR) {}
	glGenTextures(1, &out->tex);
	glBindTexture(GL_TEXTURE_2D, out->tex);
	if (bind_oes)
		bind_oes(GL_TEXTURE_2D, out->image);
	if (!bind_oes || glGetError() != GL_NO_ERROR) {
		if (!bind_storage)
			return -1;
		glDeleteTextures(1, &out->tex);
		glGenTextures(1, &out->tex);
		glBindTexture(GL_TEXTURE_2D, out->tex);
		bind_storage(GL_TEXTURE_2D, out->image, NULL);
		if (glGetError() != GL_NO_ERROR)
			return -1;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &out->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, out->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out->tex, 0);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
	       glGetError() == GL_NO_ERROR ? 0 : -1;
}

static int release_import(struct egl *e, struct import *im)
{
	int error = 0;

	if (im->fbo)
		glDeleteFramebuffers(1, &im->fbo);
	if (im->tex)
		glDeleteTextures(1, &im->tex);
	if (im->image != EGL_NO_IMAGE_KHR && !e->destroy_image(e->display, im->image))
		error = 1;
	memset(im, 0, sizeof(*im));
	im->image = EGL_NO_IMAGE_KHR;
	return error ? -1 : 0;
}

/* ------------------------------------------------------------------ child */

static GLuint shader(GLenum type, const char *source)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "FAIL shader: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

/* The same shader as hermes-gpu-scanout-test, mirrored by pattern.h. */
static GLuint pattern_program(void)
{
	const char *vs = "#version 330 core\n"
		"void main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
		"gl_Position=vec4(p*2.0-1.0,0.0,1.0); }\n";
	const char *fs = "#version 330 core\n"
		"out vec4 color; uniform uint seed; void main(){"
		"uvec2 p=uvec2(gl_FragCoord.xy);"
		"uint v=p.x*1664525u+p.y*1013904223u+seed;"
		"v^=v>>16; v*=2246822519u; v^=v>>13;"
		"color=vec4(float(v&255u),float((v>>8)&255u),"
		"float((v>>16)&255u),255.0)/255.0; }\n";
	GLuint vertex = shader(GL_VERTEX_SHADER, vs), fragment = shader(GL_FRAGMENT_SHADER, fs);
	GLuint program = 0;
	GLint ok = 0;

	if (vertex && fragment) {
		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glLinkProgram(program);
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok) {
			glDeleteProgram(program);
			program = 0;
		}
	}
	if (vertex)
		glDeleteShader(vertex);
	if (fragment)
		glDeleteShader(fragment);
	return program;
}

static int send_hello(int sock, const struct hello *hello, const int *fds, size_t nfds)
{
	char control[CMSG_SPACE(sizeof(int) * MAX_BUFFERS * MAX_PLANES)];
	struct iovec iov = {.iov_base = (void *)hello, .iov_len = sizeof(*hello)};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};

	memset(control, 0, sizeof(control));
	if (nfds) {
		msg.msg_control = control;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
	}
	return sendmsg(sock, &msg, MSG_NOSIGNAL) == (ssize_t)sizeof(*hello) ? 0 : -1;
}

static int run_producer(int sock, const struct options *o)
{
	struct egl e = {.display = EGL_NO_DISPLAY, .context = EGL_NO_CONTEXT};
	struct gbm_bo *bo[MAX_BUFFERS] = {0};
	struct import target[MAX_BUFFERS];
	struct hello hello = {.status = 1, .buffers = o->buffers, .width = o->width, .height = o->height};
	int fds[MAX_BUFFERS * MAX_PLANES];
	size_t nfds = 0;
	GLuint program = 0, vao = 0;
	int gpu_fd, result = 1;

	for (size_t i = 0; i < MAX_BUFFERS; i++) {
		target[i].image = EGL_NO_IMAGE_KHR;
		target[i].tex = target[i].fbo = 0;
	}
	gpu_fd = open(o->gpu_path, O_RDWR | O_CLOEXEC);
	if (gpu_fd < 0 || drmGetNodeTypeFromFd(gpu_fd) != DRM_NODE_RENDER ||
	    init_egl(&e, gpu_fd, o->allow_other, true))
		goto out;

	/* Renderable candidates, as the scanout experiment allocates them. */
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query =
		(PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	EGLint count = 0, returned = 0;
	if (!query || !query(e.display, FORMAT, 0, NULL, NULL, &count) || count < 1 || count > 4096)
		goto out;
	EGLuint64KHR *mods = calloc((size_t)count, sizeof(*mods));
	EGLBoolean *external = calloc((size_t)count, sizeof(*external));
	uint64_t *candidates = calloc((size_t)count, sizeof(*candidates));
	size_t ncandidates = 0;
	if (mods && external && candidates &&
	    query(e.display, FORMAT, count, mods, external, &returned) && returned > 0 && returned <= count)
		for (EGLint i = 0; i < returned; i++)
			if (!external[i] && mods[i] != DRM_FORMAT_MOD_INVALID &&
			    (!o->linear || mods[i] == DRM_FORMAT_MOD_LINEAR))
				candidates[ncandidates++] = mods[i];
	free(mods);
	free(external);
	if (!ncandidates) {
		fprintf(stderr, "FAIL: no renderable %s ARGB8888 layout on this GPU\n", o->linear ? "linear" : "modified");
		free(candidates);
		goto out;
	}
	for (uint32_t b = 0; b < o->buffers; b++) {
		bo[b] = gbm_bo_create_with_modifiers2(e.gbm, o->width, o->height, FORMAT, candidates,
			(unsigned int)ncandidates, GBM_BO_USE_RENDERING);
		if (!bo[b])
			break;
		int planes = gbm_bo_get_plane_count(bo[b]);
		uint64_t modifier = gbm_bo_get_modifier(bo[b]);
		if (planes < 1 || planes > (int)MAX_PLANES || modifier == DRM_FORMAT_MOD_INVALID ||
		    (b && ((uint32_t)planes != hello.planes || modifier != hello.modifier))) {
			fprintf(stderr, "FAIL: buffer %u layout differs from buffer 0 or is unusable\n", b);
			gbm_bo_destroy(bo[b]);
			bo[b] = NULL;
			break;
		}
		hello.planes = (uint32_t)planes;
		hello.modifier = modifier;
		int own[MAX_PLANES] = {-1, -1, -1, -1};
		for (int p = 0; p < planes; p++) {
			own[p] = gbm_bo_get_fd_for_plane(bo[b], p);
			hello.pitch[b][p] = gbm_bo_get_stride_for_plane(bo[b], p);
			hello.offset[b][p] = gbm_bo_get_offset(bo[b], p);
			if (own[p] >= 0)
				fds[nfds++] = own[p];
		}
		if ((size_t)planes * (b + 1) != nfds ||
		    import_buffer(&e, &target[b], o->width, o->height, own, hello.pitch[b],
				  hello.offset[b], hello.planes, hello.modifier))
			break;
	}
	free(candidates);
	if (nfds != (size_t)hello.planes * o->buffers || !hello.planes)
		goto out;
	program = pattern_program();
	if (!program)
		goto out;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glUseProgram(program);
	glDisable(GL_DITHER);
	glDisable(GL_BLEND);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glViewport(0, 0, (GLsizei)o->width, (GLsizei)o->height);
	const GLint seed_location = glGetUniformLocation(program, "seed");
	if (seed_location < 0 || glGetError() != GL_NO_ERROR)
		goto out;
	hello.status = 0;
	if (send_hello(sock, &hello, fds, nfds))
		goto out;
	for (;;) {
		struct command c;
		int32_t ack;
		ssize_t got = recv(sock, &c, sizeof(c), 0);

		if (got != (ssize_t)sizeof(c) || c.op == OP_QUIT)
			break;
		if (c.op != OP_RENDER || c.index >= o->buffers)
			break;
		glBindFramebuffer(GL_FRAMEBUFFER, target[c.index].fbo);
		glUniform1ui(seed_location, c.seed);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish(); /* completed before the consumer is told to sample */
		ack = glGetError() == GL_NO_ERROR ? 0 : 1;
		if (send(sock, &ack, sizeof(ack), MSG_NOSIGNAL) != (ssize_t)sizeof(ack) || ack)
			break;
	}
	result = 0;
out:
	if (hello.status) {
		hello.status = 1;
		(void)send_hello(sock, &hello, NULL, 0);
	}
	if (e.display != EGL_NO_DISPLAY && eglGetCurrentContext() == e.context) {
		if (vao)
			glDeleteVertexArrays(1, &vao);
		if (program)
			glDeleteProgram(program);
		for (uint32_t b = 0; b < MAX_BUFFERS; b++)
			if (release_import(&e, &target[b]))
				result = 1;
	}
	for (size_t i = 0; i < nfds; i++)
		close(fds[i]);
	for (uint32_t b = 0; b < MAX_BUFFERS; b++)
		if (bo[b])
			gbm_bo_destroy(bo[b]);
	if (cleanup_egl(&e))
		result = 1;
	if (gpu_fd >= 0)
		close(gpu_fd);
	return result;
}

/* --------------------------------------------------------------- consumer */

static int receive_hello(int sock, struct hello *hello, int *fds, size_t max_fds, size_t *nfds)
{
	char control[CMSG_SPACE(sizeof(int) * MAX_BUFFERS * MAX_PLANES)];
	struct iovec iov = {.iov_base = hello, .iov_len = sizeof(*hello)};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control)};
	ssize_t got;

	*nfds = 0;
	do {
		got = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	} while (got < 0 && errno == EINTR);
	if (got != (ssize_t)sizeof(*hello))
		return -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;
		size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < n; i++) {
			int fd;

			memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
			if (*nfds < max_fds)
				fds[(*nfds)++] = fd;
			else
				close(fd);
		}
	}
	return (msg.msg_flags & MSG_CTRUNC) ? -1 : 0;
}

static int render_remote(int sock, uint32_t index, uint32_t seed)
{
	struct command c = {.op = OP_RENDER, .index = index, .seed = seed};
	int32_t ack = 1;

	if (send(sock, &c, sizeof(c), MSG_NOSIGNAL) != (ssize_t)sizeof(c) ||
	    recv(sock, &ack, sizeof(ack), 0) != (ssize_t)sizeof(ack))
		return -1;
	return ack ? -1 : 0;
}

/* Blit an import into the own RGBA8 target and read it back. *sample_ms
 * (optional) receives the GPU blit time, excluding the readback. */
static int sample(const struct import *im, GLuint target_fbo, uint32_t w, uint32_t h,
		  unsigned char *pixels, double *sample_ms)
{
	const double start = now_ms();

	glBindFramebuffer(GL_READ_FRAMEBUFFER, im->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
	glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)w, (GLint)h,
			  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glFinish();
	if (sample_ms)
		*sample_ms = now_ms() - start;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	return glGetError() == GL_NO_ERROR ? 0 : -1;
}

enum verdict { V_CURRENT, V_STALE, V_CORRUPT, V_ERROR };

/*
 * The two plausible stale behaviours are an image that lags one render and an
 * image frozen at what it showed when first sampled (a private copy made at
 * import), so both of those renders are recognised; anything else is CORRUPT.
 */
static enum verdict classify(const unsigned char *pixels, uint32_t w, uint32_t h,
			     uint32_t seed, size_t nold, const uint32_t *old, size_t *bad)
{
	int status = hermes_gpu_pattern_verify(pixels, w, h, seed, bad);

	if (status < 0)
		return V_ERROR;
	if (!status)
		return V_CURRENT;
	for (size_t i = 0; i < nold; i++)
		if (old[i] != seed && !hermes_gpu_pattern_verify(pixels, w, h, old[i], NULL))
			return V_STALE;
	return V_CORRUPT;
}

int main(int argc, char **argv)
{
	struct options o = {.buffers = 3, .rounds = 40, .width = 2560, .height = 1440, .fresh = true};
	static const struct option long_options[] = {
		{"gpu", required_argument, NULL, 'g'}, {"buffers", required_argument, NULL, 'b'},
		{"rounds", required_argument, NULL, 'r'}, {"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'h'}, {"modifiers", required_argument, NULL, 'm'},
		{"no-fresh", no_argument, NULL, 'n'}, {"churn", required_argument, NULL, 'c'},
		{"allow-other-gpu", no_argument, NULL, 'a'}, {"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
		switch (opt) {
		case 'g': o.gpu_path = optarg; break;
		case 'b': if (number(optarg, &o.buffers, 1, MAX_BUFFERS)) goto invalid; break;
		case 'r': if (number(optarg, &o.rounds, 2, 10000)) goto invalid; break;
		case 'w': if (number(optarg, &o.width, 1, 16384)) goto invalid; break;
		case 'h': if (number(optarg, &o.height, 1, 16384)) goto invalid; break;
		case 'c': if (number(optarg, &o.churn, 0, 100000)) goto invalid; break;
		case 'n': o.fresh = false; break;
		case 'a': o.allow_other = true; break;
		case 'm':
			if (!strcmp(optarg, "egl")) o.linear = false;
			else if (!strcmp(optarg, "linear")) o.linear = true;
			else goto invalid;
			break;
		case '?':
			if (!strcmp(argv[optind - 1], "--help")) { usage(stdout, argv[0]); return 0; }
			goto invalid;
		default: goto invalid;
		}
	}
	if (!o.gpu_path || optind != argc)
		goto invalid;
	setvbuf(stdout, NULL, _IOLBF, 0);
	hermes_nvidia_print_version(stdout);

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
		perror("socketpair");
		return 1;
	}
	/* Fork before either side touches EGL/GL, so each has its own driver state. */
	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		perror("fork");
		return 1;
	}
	if (!child) {
		close(sv[0]);
		int status = run_producer(sv[1], &o);
		close(sv[1]);
		_exit(status);
	}
	close(sv[1]);
	signal(SIGPIPE, SIG_IGN);

	struct egl e = {.display = EGL_NO_DISPLAY, .context = EGL_NO_CONTEXT};
	struct import cached[MAX_BUFFERS], fresh;
	struct hello hello;
	int fds[MAX_BUFFERS * MAX_PLANES];
	size_t nfds = 0;
	GLuint target_tex = 0, target_fbo = 0;
	unsigned char *pixels = NULL;
	double *cold = calloc(MAX_SAMPLES, sizeof(double));
	double *warm = calloc(MAX_SAMPLES, sizeof(double));
	double *fresh_ms = calloc(MAX_SAMPLES, sizeof(double));
	size_t ncold = 0, nwarm = 0, nfresh_ms = 0;
	uint64_t cold_bad = 0, current = 0, stale = 0, corrupt = 0, fresh_ok = 0, fresh_bad = 0;
	int gpu_fd = -1, result = 1, child_status = -1;
	char text[192];

	for (size_t i = 0; i < MAX_BUFFERS; i++) {
		memset(&cached[i], 0, sizeof(cached[i]));
		cached[i].image = EGL_NO_IMAGE_KHR;
	}
	memset(&fresh, 0, sizeof(fresh));
	fresh.image = EGL_NO_IMAGE_KHR;

	if (!cold || !warm || !fresh_ms ||
	    receive_hello(sv[0], &hello, fds, sizeof(fds) / sizeof(fds[0]), &nfds) || hello.status) {
		fprintf(stderr, "FAIL: the producer process could not allocate and export its buffers\n");
		goto out;
	}
	if (hello.buffers != o.buffers || hello.width != o.width || hello.height != o.height ||
	    !hello.planes || hello.planes > MAX_PLANES || nfds != (size_t)hello.planes * hello.buffers) {
		fprintf(stderr, "FAIL: malformed producer description\n");
		goto out;
	}
	hermes_mod_describe(hello.modifier, text, sizeof(text));
	printf("producer: %u buffers %ux%u ARGB8888 modifier=0x%016" PRIx64 " (%s) planes=%u, another process\n",
	       hello.buffers, hello.width, hello.height, hello.modifier, text, hello.planes);

	gpu_fd = open(o.gpu_path, O_RDWR | O_CLOEXEC);
	if (gpu_fd < 0 || init_egl(&e, gpu_fd, o.allow_other, false))
		goto out;
	glGenTextures(1, &target_tex);
	glBindTexture(GL_TEXTURE_2D, target_tex);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)o.width, (GLsizei)o.height);
	glGenFramebuffers(1, &target_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
	pixels = malloc((size_t)o.width * o.height * 4);
	if (!pixels || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		goto out;

	uint32_t previous[MAX_BUFFERS] = {0}, imported[MAX_BUFFERS] = {0};
	for (uint32_t r = 0; r < o.rounds; r++) {
		for (uint32_t b = 0; b < o.buffers; b++) {
			const uint32_t seed = HERMES_GPU_PATTERN_SEED + r * o.buffers + b;
			const int *plane_fds = &fds[b * hello.planes];
			double ms = 0.0;
			size_t bad = 0;

			if (render_remote(sv[0], b, seed)) {
				fprintf(stderr, "FAIL: producer render request %u/%u\n", r, b);
				goto out;
			}
			if (cached[b].image == EGL_NO_IMAGE_KHR) {
				const double start = now_ms();
				double blit_ms = 0.0;
				if (import_buffer(&e, &cached[b], o.width, o.height, plane_fds, hello.pitch[b],
						  hello.offset[b], hello.planes, hello.modifier))
					goto out;
				const double import_ms = now_ms() - start;
				if (sample(&cached[b], target_fbo, o.width, o.height, pixels, &blit_ms))
					goto out;
				ms = import_ms + blit_ms;
				if (ncold < MAX_SAMPLES) cold[ncold++] = ms;
				if (classify(pixels, o.width, o.height, seed, 0, NULL, &bad) != V_CURRENT) {
					fprintf(stderr, "FAIL: first import of buffer %u is wrong at byte %zu\n", b, bad);
					cold_bad++;
				}
				imported[b] = seed;
			} else {
				if (sample(&cached[b], target_fbo, o.width, o.height, pixels, &ms))
					goto out;
				if (nwarm < MAX_SAMPLES) warm[nwarm++] = ms;
				const uint32_t old[2] = {previous[b], imported[b]};
				switch (classify(pixels, o.width, o.height, seed, 2, old, &bad)) {
				case V_CURRENT:
					current++;
					break;
				case V_STALE:
					if (!stale)
						fprintf(stderr, "STALE: cached import of buffer %u shows an earlier render in round %u\n",
							b, r);
					stale++;
					break;
				case V_CORRUPT:
					if (!corrupt)
						fprintf(stderr, "CORRUPT: cached import of buffer %u, round %u, first bad byte %zu\n",
							b, r, bad);
					corrupt++;
					break;
				default:
					goto out;
				}
				if (o.fresh) {
					const double start = now_ms();
					double blit_ms = 0.0;
					if (import_buffer(&e, &fresh, o.width, o.height, plane_fds, hello.pitch[b],
							  hello.offset[b], hello.planes, hello.modifier))
						goto out;
					const double import_ms = now_ms() - start;
					if (sample(&fresh, target_fbo, o.width, o.height, pixels, &blit_ms) ||
					    release_import(&e, &fresh))
						goto out;
					if (nfresh_ms < MAX_SAMPLES) fresh_ms[nfresh_ms++] = import_ms + blit_ms;
					if (classify(pixels, o.width, o.height, seed, 0, NULL, &bad) == V_CURRENT)
						fresh_ok++;
					else
						fresh_bad++;
				}
			}
			previous[b] = seed;
		}
	}

	if (o.churn) {
		printf("churn: %u import/sample/destroy cycles of buffer 0 (watch the GPU memory use now)\n", o.churn);
		fflush(stdout);
		const double start = now_ms();
		for (uint32_t i = 0; i < o.churn; i++) {
			if (import_buffer(&e, &fresh, o.width, o.height, &fds[0], hello.pitch[0],
					  hello.offset[0], hello.planes, hello.modifier) ||
			    sample(&fresh, target_fbo, o.width, o.height, pixels, NULL) ||
			    release_import(&e, &fresh))
				goto out;
		}
		printf("churn: %.3f ms per cycle\n", (now_ms() - start) / o.churn);
	}
	result = 0;
out:
	if (result) {
		fprintf(stderr, "FAIL: experiment incomplete (EGL 0x%x)\n", eglGetError());
	}
	{
		struct command quit = {.op = OP_QUIT};
		(void)send(sv[0], &quit, sizeof(quit), MSG_NOSIGNAL);
	}
	close(sv[0]);
	if (waitpid(child, &child_status, 0) != child ||
	    !WIFEXITED(child_status) || WEXITSTATUS(child_status)) {
		fprintf(stderr, "FAIL: producer process exit status 0x%x\n", child_status);
		result = 1;
	}
	if (e.display != EGL_NO_DISPLAY && eglGetCurrentContext() == e.context) {
		if (release_import(&e, &fresh))
			result = 1;
		for (uint32_t b = 0; b < MAX_BUFFERS; b++)
			if (release_import(&e, &cached[b]))
				result = 1;
		if (target_fbo)
			glDeleteFramebuffers(1, &target_fbo);
		if (target_tex)
			glDeleteTextures(1, &target_tex);
	}
	if (cleanup_egl(&e)) {
		fprintf(stderr, "FAIL: GL/EGL teardown\n");
		result = 1;
	}
	for (size_t i = 0; i < nfds; i++)
		close(fds[i]);
	if (gpu_fd >= 0)
		close(gpu_fd);
	free(pixels);

	const uint64_t resamples = current + stale + corrupt;
	printf("\nfirst imports:        %zu (%" PRIu64 " wrong), median %.3f ms import+sample\n",
	       ncold, cold_bad, median(cold, ncold));
	printf("cached re-samples:    %" PRIu64 ": %" PRIu64 " current, %" PRIu64 " stale, %" PRIu64 " corrupt; median %.3f ms\n",
	       resamples, current, stale, corrupt, median(warm, nwarm));
	if (o.fresh)
		printf("fresh re-imports:     %" PRIu64 ": %" PRIu64 " current, %" PRIu64 " wrong; median %.3f ms import+sample\n",
		       fresh_ok + fresh_bad, fresh_ok, fresh_bad, median(fresh_ms, nfresh_ms));
	free(cold);
	free(warm);
	free(fresh_ms);

	if (!result && (stale || corrupt || cold_bad || fresh_bad))
		result = 1;
	if (result) {
		printf("RESULT: FAIL\n");
		if (cold_bad)
			printf("        Imports of this layout read wrong pixels before any rewrite: an import\n"
			       "        correctness problem (compare hermes-egl-import-check), not caching.\n"
			       "        Keep the CPU copy for this layout.\n");
		else if ((stale || corrupt) && !fresh_bad)
			printf("        Cached images return old or damaged content while new imports are\n"
			       "        correct: run Hermes with HERMES_CUDA_EGL_IMPORT_CACHE=0 on this driver.\n");
		else if (fresh_bad)
			printf("        Even new imports of a rewritten buffer can be wrong: per-frame imports\n"
			       "        do not fix this driver either; keep the CPU copy.\n");
	} else if (!resamples) {
		printf("RESULT: INCOMPLETE (no cached re-sample ran)\n");
		result = 2;
	} else {
		printf("RESULT: PASS, cached cross-process imports showed every completed render\n"
		       "        (fence scheduling, NVENC and VRAM residency are NOT TESTED)\n");
	}
	return result;
invalid:
	usage(stderr, argv[0]);
	return 2;
}
