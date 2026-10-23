// SPDX-License-Identifier: GPL-2.0
// GPU-origin PRIME passthrough diagnostic. No CPU upload or mmap of source BOs.
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES 1
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "../hermes_session.h"
#include "pattern.h"

#define FORMAT DRM_FORMAT_ARGB8888
#define SEED UINT32_C(0x3917ab25)
static volatile sig_atomic_t interrupted;
static void stop(int sig) { interrupted = sig; }

struct gpu {
	struct gbm_device *gbm;
	struct gbm_bo *bo;
	EGLDisplay display;
	EGLContext context;
	EGLImageKHR image[2];
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	GLuint tex[3], fbo[3], program, vao;
	unsigned int planes;
	int fd[4];
	uint32_t pitch[4], offset[4];
	uint64_t modifier;
};

static int has_extension(const char *list, const char *name)
{
	size_t n = strlen(name);
	const char *p = list;
	while (p && (p = strstr(p, name))) {
		if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
		p += n;
	}
	return 0;
}

static int number(const char *s, uint32_t *v, uint32_t max)
{
	char *end;
	errno = 0;
	unsigned long n = strtoul(s, &end, 10);
	if (!*s || *end || errno || n > max || s[0] == '-') return -1;
	*v = (uint32_t)n;
	return 0;
}

static void usage(FILE *f, const char *name)
{
	fprintf(f, "Usage: %s --device /dev/dri/cardN --gpu /dev/dri/renderDN [options]\n"
		"  --width N --height N   default 2560x1440, SDR ARGB8888\n"
		"  --hold-seconds N       retain static scanout for 0..3600 seconds\n"
		"  --session-file PATH    publish mode-0600 capture credential during hold\n"
		"  --allow-other-gpu      permit a non-NVIDIA hardware control test\n"
		"Requires an unused, single-output Hermes-KMS card; never steals a master.\n"
		"Exit 0 proves identity/layout/pixels and teardown, not NVENC or VRAM residency.\n", name);
}

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

static int render_pattern(struct gpu *g, uint32_t w, uint32_t h)
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
	if (!vertex || !fragment) {
		if (vertex) glDeleteShader(vertex);
		if (fragment) glDeleteShader(fragment);
		return -1;
	}
	g->program = glCreateProgram();
	glAttachShader(g->program, vertex);
	glAttachShader(g->program, fragment);
	glLinkProgram(g->program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint ok = 0;
	glGetProgramiv(g->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(g->program, sizeof(log), NULL, log);
		fprintf(stderr, "FAIL program link: %s\n", log);
		return -1;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[0]);
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glDisable(GL_DITHER);
	glDisable(GL_BLEND);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(g->program);
	glUniform1ui(glGetUniformLocation(g->program, "seed"), SEED);
	glGenVertexArrays(1, &g->vao);
	glBindVertexArray(g->vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// Deliberately conservative: this diagnostic tests storage/identity, not
	// compositor fence scheduling. Never rewrite the BO while it is scanned out.
	glFinish();
	return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static int import_image(struct gpu *g, int slot, uint32_t w, uint32_t h,
	const int fd[4])
{
	static const EGLint keys[4][5] = {
		{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
		 EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		 EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
		 EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
		{EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		 EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}
	};
	EGLint a[48] = {EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h,
		EGL_LINUX_DRM_FOURCC_EXT, FORMAT};
	size_t n = 6;
	for (unsigned int i = 0; i < g->planes; i++) {
		if (g->pitch[i] > INT_MAX || g->offset[i] > INT_MAX) return -1;
		EGLint values[5] = {fd[i], (EGLint)g->offset[i], (EGLint)g->pitch[i],
			(EGLint)(uint32_t)g->modifier, (EGLint)(uint32_t)(g->modifier >> 32)};
		for (unsigned int k = 0; k < 5; k++) { a[n++] = keys[i][k]; a[n++] = values[k]; }
	}
	a[n] = EGL_NONE;
	g->image[slot] = g->create_image(g->display, EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT, NULL, a);
	if (g->image[slot] == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "FAIL EGL import slot %d: 0x%x\n", slot, eglGetError());
		return -1;
	}
	typedef void (GLAPIENTRY *bind_oes_t)(GLenum, void *);
	typedef void (GLAPIENTRY *bind_storage_t)(GLenum, void *, const GLint *);
	bind_oes_t bind_oes = (bind_oes_t)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	bind_storage_t bind_storage = (bind_storage_t)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
	glGenTextures(1, &g->tex[slot]);
	glBindTexture(GL_TEXTURE_2D, g->tex[slot]);
	if (bind_oes) bind_oes(GL_TEXTURE_2D, g->image[slot]);
	GLenum error = glGetError();
	if (!bind_oes || error != GL_NO_ERROR) {
		if (!bind_storage) return -1;
		glDeleteTextures(1, &g->tex[slot]);
		glGenTextures(1, &g->tex[slot]);
		glBindTexture(GL_TEXTURE_2D, g->tex[slot]);
		bind_storage(GL_TEXTURE_2D, g->image[slot], NULL);
		if (glGetError() != GL_NO_ERROR) return -1;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &g->fbo[slot]);
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[slot]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex[slot], 0);
	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
		glGetError() == GL_NO_ERROR ? 0 : -1;
}

static int init_gpu(struct gpu *g, int fd, uint32_t w, uint32_t h, int allow_other)
{
	drmVersionPtr version = drmGetVersion(fd);
	int nvidia = version && version->name && strcmp(version->name, "nvidia-drm") == 0;
	printf("GPU DRM driver: %s\n", version && version->name ? version->name : "unknown");
	if (version) drmFreeVersion(version);
	if (!nvidia && !allow_other) { fprintf(stderr, "FAIL: GPU is not nvidia-drm\n"); return -1; }
	g->gbm = gbm_create_device(fd);
	if (!g->gbm) return -1;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display) return -1;
	g->display = get_display(EGL_PLATFORM_GBM_KHR, g->gbm, NULL);
	if (g->display == EGL_NO_DISPLAY || !eglInitialize(g->display, NULL, NULL)) return -1;
	const char *extensions = eglQueryString(g->display, EGL_EXTENSIONS);
	if (!has_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers") ||
	    !has_extension(extensions, "EGL_KHR_no_config_context") ||
	    !has_extension(extensions, "EGL_KHR_surfaceless_context")) return -1;
	g->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	g->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	if (!g->create_image || !g->destroy_image || !eglBindAPI(EGL_OPENGL_API)) return -1;
	const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
	g->context = eglCreateContext(g->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (g->context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context)) return -1;
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	printf("GL vendor: %s; renderer: %s\n", vendor ? vendor : "unknown", renderer ? renderer : "unknown");
	if (!vendor || !renderer || (!allow_other && !strstr(vendor, "NVIDIA")) ||
	    strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")) return -1;
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query =
		(PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	EGLint count = 0;
	if (!query || !query(g->display, FORMAT, 0, NULL, NULL, &count) || count < 1 || count > 4096) return -1;
	EGLuint64KHR *mods = calloc((size_t)count, sizeof(*mods));
	EGLBoolean *external = calloc((size_t)count, sizeof(*external));
	uint64_t *usable = calloc((size_t)count, sizeof(*usable));
	int usable_count = 0;
	EGLint returned = 0;
	if (mods && external && usable && query(g->display, FORMAT, count, mods, external, &returned) &&
	    returned > 0 && returned <= count) {
		for (int i = 0; i < returned; i++)
			if (!external[i] && mods[i] != DRM_FORMAT_MOD_INVALID) usable[usable_count++] = mods[i];
		if (usable_count) g->bo = gbm_bo_create_with_modifiers2(g->gbm, w, h,
			FORMAT, usable, (unsigned int)usable_count, GBM_BO_USE_RENDERING);
	}
	free(mods); free(external); free(usable);
	if (!g->bo) { fprintf(stderr, "FAIL: no explicitly modified, renderable GBM BO\n"); return -1; }
	int planes = gbm_bo_get_plane_count(g->bo);
	if (planes < 1 || planes > 4) return -1;
	g->planes = (unsigned int)planes;
	g->modifier = gbm_bo_get_modifier(g->bo);
	if (g->modifier == DRM_FORMAT_MOD_INVALID) return -1;
	printf("GPU-origin BO: %ux%u ARGB8888 modifier=0x%016" PRIx64 " planes=%u\n", w, h, g->modifier, g->planes);
	for (unsigned int i = 0; i < g->planes; i++) {
		g->fd[i] = gbm_bo_get_fd_for_plane(g->bo, (int)i);
		g->pitch[i] = gbm_bo_get_stride_for_plane(g->bo, (int)i);
		g->offset[i] = gbm_bo_get_offset(g->bo, (int)i);
		if (g->fd[i] < 0 || !g->pitch[i]) return -1;
		printf("  plane %u: pitch=%u offset=%u\n", i, g->pitch[i], g->offset[i]);
	}
	return import_image(g, 0, w, h, g->fd);
}

static int verify_pixels(struct gpu *g, int slot, uint32_t w, uint32_t h)
{
	// Read our own target after a GPU blit, never mmap a potentially tiled BO.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo[slot]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g->fbo[2]);
	glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)w, (GLint)h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g->fbo[2]);
	size_t bytes = (size_t)w * h * 4, bad = 0;
	unsigned char *pixels = malloc(bytes);
	if (!pixels) return -1;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	int error = glGetError() != GL_NO_ERROR;
	int mismatch = error ? -1 : hermes_gpu_pattern_verify(pixels, w, h, SEED, &bad);
	free(pixels);
	if (mismatch) fprintf(stderr, "FAIL full-frame pixels slot %d: status=%d first_bad_byte=%zu\n", slot, mismatch, bad);
	else printf("PASS full-frame pixels slot %d: %zu bytes%s\n", slot, bytes,
		bytes > 2 * 1024 * 1024 ? " (including suffix beyond 2 MiB)" : "");
	return mismatch ? -1 : 0;
}

static int cleanup_gpu(struct gpu *g)
{
	int error = 0;
	if (g->context != EGL_NO_CONTEXT && eglGetCurrentContext() == g->context) {
		glFinish();
		glDeleteFramebuffers(3, g->fbo);
		glDeleteTextures(3, g->tex);
		if (g->vao) glDeleteVertexArrays(1, &g->vao);
		if (g->program) glDeleteProgram(g->program);
		if (glGetError() != GL_NO_ERROR) error = 1;
	}
	if (g->display != EGL_NO_DISPLAY) {
		for (int i = 0; i < 2; i++)
			if (g->image[i] != EGL_NO_IMAGE_KHR && !g->destroy_image(g->display, g->image[i])) error = 1;
		if (!eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) error = 1;
		if (g->context != EGL_NO_CONTEXT && !eglDestroyContext(g->display, g->context)) error = 1;
		if (!eglTerminate(g->display)) error = 1;
	}
	for (int i = 0; i < 4; i++) if (g->fd[i] >= 0) close(g->fd[i]);
	if (g->bo) gbm_bo_destroy(g->bo);
	if (g->gbm) gbm_device_destroy(g->gbm);
	return error ? -1 : 0;
}

int main(int argc, char **argv)
{
	const char *card_path = NULL, *gpu_path = NULL, *session_path = NULL;
	uint32_t w = 2560, h = 1440, hold = 0;
	int allow_other = 0, opt;
	static const struct option options[] = {
		{"device", required_argument, NULL, 'd'}, {"gpu", required_argument, NULL, 'g'},
		{"width", required_argument, NULL, 'w'}, {"height", required_argument, NULL, 'h'},
		{"hold-seconds", required_argument, NULL, 't'}, {"session-file", required_argument, NULL, 's'},
		{"allow-other-gpu", no_argument, NULL, 'a'}, {"help", no_argument, NULL, '?'}, {NULL, 0, NULL, 0}
	};
	while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (opt) {
		case 'd': card_path = optarg; break;
		case 'g': gpu_path = optarg; break;
		case 'w': if (number(optarg, &w, 16384) || !w) goto invalid; break;
		case 'h': if (number(optarg, &h, 16384) || !h) goto invalid; break;
		case 't': if (number(optarg, &hold, 3600)) goto invalid; break;
		case 's': session_path = optarg; break;
		case 'a': allow_other = 1; break;
		case '?': if (!strcmp(argv[optind - 1], "--help")) { usage(stdout, argv[0]); return 0; } goto invalid;
		default: goto invalid;
		}
	}
	if (!card_path || !gpu_path || optind != argc || (session_path && !hold) ||
	    (size_t)w > SIZE_MAX / 4 / h) goto invalid;
	struct gpu g = {.display = EGL_NO_DISPLAY, .context = EGL_NO_CONTEXT,
		.image = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR}, .fd = {-1, -1, -1, -1}};
	struct drm_hermes_kms_acquire_frame frame = {.dma_buf_fd = {-1, -1, -1, -1}, .sync_file_fd = -1};
	struct hermes_session_credentials credentials = {0};
	struct drm_hermes_kms_caps caps = {0};
	struct drm_hermes_kms_status status = {0};
	struct drm_hermes_kms_set_output output = {.enabled = 1, .width = w, .height = h, .refresh_hz = 60};
	int card = -1, gpu_fd = -1, render = -1, result = 1;
	int master = 0, owned = 0, scanout = 0, session_created = 0;
	uint32_t fb = 0, handles[4] = {0};
	drmModeConnector *connector = NULL;
	struct sigaction action = {.sa_handler = stop};
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) goto out;
	card = open(card_path, O_RDWR | O_CLOEXEC);
	if (card < 0 || hermes_session_require_driver(card) || hermes_session_require_token_uapi(card)) goto out;
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_GET_CAPS, &caps) || caps.output_count != 1) {
		fprintf(stderr, "FAIL: requires a dedicated single-output Hermes-KMS card\n"); goto out;
	}
	if (drmSetMaster(card)) { fprintf(stderr, "FAIL: card already has a DRM master\n"); goto out; }
	master = 1;
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status) ||
	    (status.flags & (HERMES_KMS_STATUS_OUTPUT_ENABLED | HERMES_KMS_STATUS_SESSION_OWNED |
		HERMES_KMS_STATUS_SCANOUT_ACTIVE | HERMES_KMS_STATUS_FRAME_VALID))) {
		fprintf(stderr, "FAIL: card is not idle; no output changed\n"); goto out;
	}
	// Finish GPU setup before publishing any connected output.
	gpu_fd = open(gpu_path, O_RDWR | O_CLOEXEC);
	if (gpu_fd < 0 || drmGetNodeTypeFromFd(gpu_fd) != DRM_NODE_RENDER ||
	    init_gpu(&g, gpu_fd, w, h, allow_other) || render_pattern(&g, w, h)) goto out;
	glGenTextures(1, &g.tex[2]);
	glBindTexture(GL_TEXTURE_2D, g.tex[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &g.fbo[2]);
	glBindFramebuffer(GL_FRAMEBUFFER, g.fbo[2]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.tex[2], 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || verify_pixels(&g, 0, w, h)) goto out;
	for (unsigned int i = 0; i < g.planes; i++)
		if (drmPrimeFDToHandle(card, g.fd[i], &handles[i])) goto out;
	uint64_t modifiers[4] = {g.modifier, g.modifier, g.modifier, g.modifier};
	if (drmModeAddFB2WithModifiers(card, w, h, FORMAT, handles, g.pitch, g.offset,
		modifiers, &fb, DRM_MODE_FB_MODIFIERS)) goto out;
	if (interrupted || ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &output)) goto out;
	owned = 1;
	if (ioctl(card, DRM_IOCTL_HERMES_KMS_GET_STATUS, &status)) goto out;
	connector = drmModeGetConnector(card, status.connector_id);
	if (!connector) goto out;
	drmModeModeInfo *mode = NULL;
	for (int i = 0; i < connector->count_modes; i++)
		if (connector->modes[i].hdisplay == w && connector->modes[i].vdisplay == h &&
		    connector->modes[i].vrefresh == 60) { mode = &connector->modes[i]; break; }
	if (!mode || drmModeSetCrtc(card, status.crtc_id, fb, 0, 0, &status.connector_id, 1, mode)) goto out;
	scanout = 1;
	if (hermes_session_get_owner_token(card, &credentials)) goto out;
	render = hermes_session_open_bound_render_credentials(&credentials, NULL, 0, NULL);
	if (render < 0) goto out;
	frame.flags = HERMES_KMS_FRAME_REQUEST_DMABUF | HERMES_KMS_FRAME_REQUEST_SYNC_FILE;
	if (ioctl(render, DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME, &frame)) goto out;
	if (!(frame.flags & HERMES_KMS_FRAME_DMABUF_VALID) ||
	    !(frame.flags & HERMES_KMS_FRAME_SYNC_FILE_VALID) ||
	    (frame.flags & HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED) ||
	    frame.framebuffer_id != fb || frame.width != w || frame.height != h ||
	    frame.format != FORMAT || frame.modifier != g.modifier || frame.plane_count != g.planes ||
	    frame.sync_file_fd < 0 || hermes_sync_file_wait(frame.sync_file_fd, 5000)) {
		fprintf(stderr, "FAIL: acquired frame metadata/fence/fallback\n"); goto out;
	}
	for (unsigned int i = 0; i < g.planes; i++) {
		struct stat original, returned;
		if (frame.dma_buf_fd[i] < 0 || frame.pitch[i] != g.pitch[i] || frame.offset[i] != g.offset[i] ||
		    fstat(g.fd[i], &original) || fstat(frame.dma_buf_fd[i], &returned) ||
		    original.st_dev != returned.st_dev || original.st_ino != returned.st_ino) {
			fprintf(stderr, "FAIL: plane %u identity/layout differs\n", i); goto out;
		}
	}
	printf("PASS: original DMA-BUF identity and layout preserved on all %u planes\n", g.planes);
	if (import_image(&g, 1, w, h, frame.dma_buf_fd) || verify_pixels(&g, 1, w, h)) goto out;
	if (session_path) {
		if (hermes_session_write_file(session_path, &credentials)) goto out;
		session_created = 1;
	}
	hermes_session_forget(&credentials);
	if (hold) {
		printf("READY: static scanout held for %u seconds; consumer may attach now\n", hold);
		fflush(stdout);
		struct timespec until;
		if (clock_gettime(CLOCK_MONOTONIC, &until)) goto out;
		until.tv_sec += hold;
		while (!interrupted) {
			struct timespec now, pause = {.tv_sec = 0, .tv_nsec = 100000000};
			if (clock_gettime(CLOCK_MONOTONIC, &now)) goto out;
			if (now.tv_sec > until.tv_sec || (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)) break;
			nanosleep(&pause, NULL);
		}
	}
	result = interrupted ? 128 + interrupted : 0;
out:
	if (result == 1) fprintf(stderr, "FAIL: GPU scanout experiment: %s (EGL 0x%x)\n", strerror(errno), eglGetError());
	hermes_session_forget(&credentials);
	if (session_created && unlink(session_path)) { perror("remove session credential"); result = 1; }
	if (render >= 0) close(render);
	if (scanout && drmModeSetCrtc(card, status.crtc_id, 0, 0, 0, NULL, 0, NULL)) { perror("disable CRTC"); result = 1; }
	if (owned) {
		struct drm_hermes_kms_set_output disable = {0};
		if (ioctl(card, DRM_IOCTL_HERMES_KMS_SET_OUTPUT, &disable)) { perror("disable output"); result = 1; }
	}
	if (fb && drmModeRmFB(card, fb)) { perror("remove framebuffer"); result = 1; }
	for (unsigned int i = 0; i < 4; i++) {
		int duplicate = 0;
		for (unsigned int j = 0; j < i; j++) if (handles[i] == handles[j]) duplicate = 1;
		if (handles[i] && !duplicate && drmCloseBufferHandle(card, handles[i])) { perror("close imported handle"); result = 1; }
	}
	if (connector) drmModeFreeConnector(connector);
	if (master && drmDropMaster(card)) { perror("drop master"); result = 1; }
	if (card >= 0) close(card);
	if (cleanup_gpu(&g)) { fprintf(stderr, "FAIL: GL/EGL teardown\n"); result = 1; }
	for (int i = 0; i < 4; i++) if (frame.dma_buf_fd[i] >= 0) close(frame.dma_buf_fd[i]);
	if (frame.sync_file_fd >= 0) close(frame.sync_file_fd);
	if (gpu_fd >= 0) close(gpu_fd);
	if (!result) printf("PASS: GPU-origin PRIME passthrough, full pixels, real teardown\n"
		"NVENC encode/decode and physical memory residency: NOT TESTED\n");
	return result;
invalid:
	usage(stderr, argv[0]);
	return 2;
}
