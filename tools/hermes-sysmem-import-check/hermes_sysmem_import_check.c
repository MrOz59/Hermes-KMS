// SPDX-License-Identifier: GPL-2.0
// hermes-sysmem-import-check: can this GPU import a plain system-memory
// DMA-BUF at all?
//
// This is the Hermes-free control for hermes-egl-import-check. It builds a
// DMA-BUF out of a memfd through udmabuf - ordinary system RAM, with no GPU,
// no virtual display driver and no compositor anywhere in the picture - then
// asks EGL to import it with the geometry, format and modifier a Hermes-KMS
// scanout frame carries.
//
// The point is to split a failing import into two very different causes:
//
//   FAIL here -> the GPU stack cannot import foreign system-memory DMA-BUFs
//                for that geometry. Hermes-KMS is not involved and the same
//                failure would hit any producer of such buffers.
//   PASS here, but hermes-egl-import-check fails -> something specific to the
//                buffers Hermes-KMS exports, and the driver is worth looking at.
//
// Usage: hermes-sysmem-import-check [--verify] [--thp | --hugetlb] [--gpu PATH]
//                                   [WIDTH HEIGHT [PITCH_BYTES]]
//
// --verify   Read the whole imported image back through the GPU and compare it
//            with the buffer. NVIDIA has been seen to import these buffers
//            without error and then sample the wrong pages past the first
//            ~2 MiB; a successful eglCreateImage alone does not rule that out.
//            Every 16-byte slot of the buffer holds its own page number, so a
//            mismatch reports where the correct prefix ends and which page the
//            GPU read instead.
// --thp      Ask for 2 MiB transparent huge pages behind the memfd (the same
//            kind of backing Hermes-KMS's huge_gem parameter gives its buffers).
//            Needs shmem THP enabled; the report says how much of the buffer
//            actually got them.
// --hugetlb  Back the memfd with reserved 2 MiB hugetlb pages instead: always
//            2 MiB contiguous, but pages must be reserved first, as root:
//            echo 64 > /proc/sys/vm/nr_hugepages
// --gpu      Render node to import into (default: the first one that is
//            neither Hermes-KMS nor EVDI).
//
// Comparing --verify with and without --thp/--hugetlb tells whether an
// importer only reads physically contiguous 2 MiB runs correctly.
//
// The optional pitch is what makes this useful as a self-test. radeonsi wants
// a linear surface's pitch aligned to 256 bytes (64 pixels at 4 bytes each) and
// refuses the import otherwise, which is reported as EGL_BAD_ALLOC - the same
// error a genuinely broken import produces. Passing a deliberately misaligned
// pitch should therefore FAIL on a healthy system:
//
//   hermes-sysmem-import-check 854 480 3416   -> expected FAIL (854 % 64 = 22)
//   hermes-sysmem-import-check 854 480 3584   -> expected PASS (896 % 64 = 0)
//
// Hermes-KMS pads its backing pitch to 256 bytes precisely to stay on the
// passing side of that rule, so the first line is not a bug report - it is how
// you confirm the tool can see the failure it is looking for.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#include <linux/udmabuf.h>
#include <linux/memfd.h>
#include <xf86drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#define FOURCC_XR24 0x34325258u

static bool parse_positive_uint(const char *text, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (!text || !*text || *text == '-')
		return false;
	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !end || *end || !parsed || parsed > UINT_MAX)
		return false;
	*value = (unsigned int)parsed;
	return true;
}

static int open_real_gpu(char *name, size_t n)
{
	/* Cover the complete render-node minor range, not just the first 12. */
	for (int i = 128; i <= 255; i++) {
		char p[64];
		snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr v = drmGetVersion(fd);
		int ok = v && v->name && strcmp(v->name, "hermes-kms") != 0 &&
			 strcmp(v->name, "evdi") != 0;
		if (v) {
			if (ok) snprintf(name, n, "%s (%s)", v->name, p);
			drmFreeVersion(v);
		}
		if (ok)
			return fd;
		close(fd);
	}
	return -1;
}

enum backing {
	BACKING_PAGES,
	BACKING_THP,
	BACKING_HUGETLB,
};

#define HUGE_2M ((size_t)2 << 20)

/*
 * Every 16-byte slot of the buffer names itself: blue is the slot within its
 * 4 KiB page, green and red the page number. A GPU that reads the wrong page
 * returns another page's name, which says where the read actually came from.
 */
static void pattern_at(size_t offset, uint8_t out[4])
{
	const size_t page = offset >> 12;

	out[0] = (uint8_t)((offset >> 4) & 0xff);
	out[1] = (uint8_t)(page & 0xff);
	out[2] = (uint8_t)((page >> 8) & 0xff);
	out[3] = 0xff;
}

static unsigned long read_ulong(const char *path)
{
	unsigned long value = 0;
	FILE *f = fopen(path, "r");

	if (f) {
		if (fscanf(f, "%lu", &value) != 1)
			value = 0;
		fclose(f);
	}
	return value;
}

#define THP_STATS "/sys/kernel/mm/transparent_hugepage/hugepages-2048kB/stats/"

/*
 * Compare the GPU's view (tightly packed BGRA rows) against the pattern.
 * `flipped` compares GPU row y with buffer row height-1-y.
 */
static size_t count_mismatches(const uint8_t *gpu, unsigned int width,
			       unsigned int height, unsigned int pitch,
			       bool flipped, size_t *first_offset,
			       size_t *first_gpu_index)
{
	size_t wrong = 0;

	for (unsigned int y = 0; y < height; y++) {
		const unsigned int row = flipped ? height - 1U - y : y;
		for (unsigned int x = 0; x < width; x++) {
			const size_t offset = (size_t)row * pitch + (size_t)x * 4U;
			const size_t index = ((size_t)y * width + x) * 4U;
			uint8_t expected[4];

			pattern_at(offset, expected);
			if (gpu[index] == expected[0] && gpu[index + 1] == expected[1] &&
			    gpu[index + 2] == expected[2])
				continue;
			if (!wrong || offset < *first_offset) {
				*first_offset = offset;
				*first_gpu_index = index;
			}
			wrong++;
		}
	}
	return wrong;
}

/* Bind the image as a texture, read all of it back, and report. */
static int verify_import(EGLDisplay dpy, EGLImage img, unsigned int width,
			 unsigned int height, unsigned int pitch, size_t size)
{
	int result = 1;
	GLuint tex = 0, fbo = 0;
	uint8_t *gpu = NULL;

	if (!eglBindAPI(EGL_OPENGL_API)) {
		fprintf(stderr, "FAIL     : eglBindAPI(OPENGL)\n");
		return 1;
	}
	EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
	if (ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "FAIL     : surfaceless GL context (0x%x)\n", eglGetError());
		return 1;
	}
	printf("GL       : %s\n", (const char *)glGetString(GL_RENDERER));

	typedef void (*image_target_fn)(GLenum, void *);
	typedef void (*image_storage_fn)(GLenum, void *, const GLint *);
	image_target_fn target = (image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	image_storage_fn storage = (image_storage_fn)eglGetProcAddress("glEGLImageTargetTexStorageEXT");

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	bool bound = false;
	if (target) {
		target(GL_TEXTURE_2D, img);
		bound = glGetError() == GL_NO_ERROR;
	}
	if (!bound && storage) {
		/* NVIDIA's desktop GL rejects the OES bind for foreign images. */
		glDeleteTextures(1, &tex);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		storage(GL_TEXTURE_2D, img, NULL);
		bound = glGetError() == GL_NO_ERROR;
	}
	if (!bound) {
		fprintf(stderr, "FAIL     : could not bind the EGLImage as a texture\n");
		goto out;
	}

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "FAIL     : framebuffer incomplete with the imported texture\n");
		goto out;
	}

	gpu = malloc((size_t)width * height * 4U);
	if (!gpu) {
		fprintf(stderr, "FAIL     : out of memory for the readback\n");
		goto out;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_BGRA, GL_UNSIGNED_BYTE, gpu);
	glFinish();
	if (glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "FAIL     : glReadPixels\n");
		goto out;
	}

	size_t first = 0, first_index = 0;
	size_t wrong = count_mismatches(gpu, width, height, pitch, false, &first, &first_index);
	bool flipped = false;
	if (wrong) {
		size_t f_first = 0, f_index = 0;
		size_t f_wrong = count_mismatches(gpu, width, height, pitch, true, &f_first, &f_index);
		if (f_wrong < wrong) {
			flipped = true;
			wrong = f_wrong;
			first = f_first;
			first_index = f_index;
		}
	}
	const size_t pixels = (size_t)width * height;
	if (flipped)
		printf("INFO     : the GPU returned rows bottom-up; compared that way\n");
	if (!wrong) {
		printf("PASS     : GPU readback matches all %zu pixels (%zu bytes of buffer)\n",
		       pixels, size);
		result = 0;
		goto out;
	}

	const uint8_t *got = gpu + first_index;
	const size_t got_page = (size_t)got[1] | (size_t)got[2] << 8;
	printf("FAIL     : GPU readback differs from the buffer: %zu of %zu pixels wrong\n",
	       wrong, pixels);
	printf("           correct up to byte %zu (%.2f MiB): page %zu, 2 MiB block %zu\n",
	       first, first / 1048576.0, first >> 12, first / HUGE_2M);
	if (got[3] == 0xff && got_page < (size / 4096U))
		printf("           there the GPU returned the bytes of page %zu, slot %u\n",
		       got_page, got[0]);
	else
		printf("           there the GPU returned bytes that are not from this buffer\n");

out:
	free(gpu);
	if (fbo)
		glDeleteFramebuffers(1, &fbo);
	if (tex)
		glDeleteTextures(1, &tex);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	return result;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--verify] [--thp | --hugetlb] [--gpu PATH] [WIDTH HEIGHT [PITCH_BYTES]]\n",
		argv0);
}

int main(int argc, char **argv)
{
	int import_succeeded = 0;
	int verify_failed = 0;
	bool verify = false;
	enum backing backing = BACKING_PAGES;
	const char *gpu_path = NULL;
	const char *positional[3];
	int npositional = 0;
	unsigned int width = 1600;
	unsigned int height = 1068;
	unsigned int pitch;
	long page_value;
	size_t page;
	size_t unrounded_size;
	size_t size;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verify")) {
			verify = true;
		} else if (!strcmp(argv[i], "--thp") && backing == BACKING_PAGES) {
			backing = BACKING_THP;
		} else if (!strcmp(argv[i], "--hugetlb") && backing == BACKING_PAGES) {
			backing = BACKING_HUGETLB;
		} else if (!strcmp(argv[i], "--gpu") && i + 1 < argc) {
			gpu_path = argv[++i];
		} else if (argv[i][0] != '-' && npositional < 3) {
			positional[npositional++] = argv[i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if ((npositional > 0 && !parse_positive_uint(positional[0], &width)) ||
	    (npositional > 1 && !parse_positive_uint(positional[1], &height)) ||
	    npositional == 1 || width > UINT_MAX / 4U) {
		usage(argv[0]);
		return 2;
	}
	pitch = width * 4U;
	if (npositional > 2 && !parse_positive_uint(positional[2], &pitch)) {
		usage(argv[0]);
		return 2;
	}
	if (pitch < width * 4U || pitch % 4U || (size_t)height > SIZE_MAX / (size_t)pitch) {
		fprintf(stderr, "invalid or overflowing framebuffer layout\n");
		return 2;
	}
	unrounded_size = (size_t)pitch * (size_t)height;
	page_value = sysconf(_SC_PAGESIZE);
	if (page_value <= 0 || (size_t)page_value > SIZE_MAX - unrounded_size) {
		fprintf(stderr, "could not safely determine the DMA-BUF allocation size\n");
		return 1;
	}
	/* Huge-page backing needs whole 2 MiB pages behind the buffer. */
	page = backing == BACKING_PAGES ? (size_t)page_value : HUGE_2M;
	if (page - 1U > SIZE_MAX - unrounded_size) {
		fprintf(stderr, "could not safely determine the DMA-BUF allocation size\n");
		return 1;
	}
	size = ((unrounded_size + page - 1U) / page) * page;
	if ((off_t)size < 0 || (size_t)(off_t)size != size) {
		fprintf(stderr, "DMA-BUF allocation exceeds off_t\n");
		return 2;
	}

	printf("geometry : %ux%u  XR24  pitch=%u (%u px, %%64=%u)  size=%zu (pitch*h=%zu)\n",
	       width, height, pitch, pitch / 4, (pitch / 4) % 64, size,
	       (size_t) pitch * height);

	unsigned int memfd_flags = MFD_ALLOW_SEALING | MFD_CLOEXEC;
	if (backing == BACKING_HUGETLB)
		memfd_flags |= MFD_HUGETLB | MFD_HUGE_2MB;
	int mfd = memfd_create("udmabuf-test", memfd_flags);
	if (mfd < 0) { perror("FAIL memfd_create"); return 1; }
	if (ftruncate(mfd, (off_t) size) < 0) { perror("FAIL ftruncate"); return 1; }
	if (backing == BACKING_HUGETLB && fallocate(mfd, 0, 0, (off_t)size) < 0) {
		perror("FAIL fallocate of hugetlb pages");
		fprintf(stderr, "      reserve %zu more 2 MiB pages first, as root:\n"
				"      echo N > /proc/sys/vm/nr_hugepages\n", size / HUGE_2M);
		return 1;
	}

	/* Fill through a mapping: hugetlbfs has no write(). */
	unsigned long thp_before = read_ulong(THP_STATS "shmem_alloc");
	uint8_t *fill = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (fill == MAP_FAILED) { perror("FAIL mmap"); return 1; }
	if (backing == BACKING_THP && madvise(fill, size, MADV_HUGEPAGE) < 0)
		perror("WARN madvise(MADV_HUGEPAGE)");
	for (size_t offset = 0; offset < size; offset += 4)
		pattern_at(offset, fill + offset);
	munmap(fill, size);
	if (backing == BACKING_THP) {
		unsigned long got = read_ulong(THP_STATS "shmem_alloc") - thp_before;
		printf("backing  : %lu of %zu 2 MiB blocks got transparent huge pages%s\n",
		       got, size / HUGE_2M,
		       got < size / HUGE_2M ? " (check /sys/kernel/mm/transparent_hugepage/shmem_enabled)" : "");
	} else if (backing == BACKING_HUGETLB) {
		printf("backing  : %zu reserved 2 MiB hugetlb pages\n", size / HUGE_2M);
	} else {
		printf("backing  : ordinary 4 KiB pages, wherever the allocator put them\n");
	}

	if (fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) { perror("FAIL F_SEAL_SHRINK"); return 1; }

	int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udev < 0) {
		perror("FAIL open /dev/udmabuf");
		fprintf(stderr, "      (needs access to /dev/udmabuf - try as root)\n");
		return 1;
	}

	struct udmabuf_create create = { .memfd = (uint32_t) mfd, .flags = 0,
					 .offset = 0, .size = size };
	int dbuf = ioctl(udev, UDMABUF_CREATE, &create);
	if (dbuf < 0) { perror("FAIL UDMABUF_CREATE"); return 1; }
	printf("PASS     : created a %zu byte system-memory DMA-BUF via udmabuf\n", size);

	char gpu[128] = "?";
	int gpu_fd;
	if (gpu_path) {
		gpu_fd = open(gpu_path, O_RDWR | O_CLOEXEC);
		snprintf(gpu, sizeof gpu, "%s", gpu_path);
	} else {
		gpu_fd = open_real_gpu(gpu, sizeof gpu);
	}
	if (gpu_fd < 0) { fprintf(stderr, "FAIL: no usable GPU render node\n"); return 1; }
	printf("GPU      : %s\n", gpu);

	struct gbm_device *gbm = gbm_create_device(gpu_fd);
	if (!gbm) { fprintf(stderr, "FAIL gbm_create_device\n"); return 1; }
	EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType) gbm);
	EGLint maj, min;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
		fprintf(stderr, "FAIL eglInitialize\n"); return 1;
	}
	const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
	int has_mod = ext && strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers");
	printf("EGL      : %d.%d, modifiers extension %s\n", maj, min,
	       has_mod ? "present" : "ABSENT");

	for (int pass = 0; pass < 2; pass++) {
		int with_mod = (pass == 0) && has_mod;
		EGLAttrib a[32];
		int i = 0;
		a[i++] = EGL_WIDTH;                     a[i++] = width;
		a[i++] = EGL_HEIGHT;                    a[i++] = height;
		a[i++] = EGL_LINUX_DRM_FOURCC_EXT;      a[i++] = FOURCC_XR24;
		a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;     a[i++] = dbuf;
		a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[i++] = 0;
		a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  a[i++] = pitch;
		if (with_mod) {
			a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; a[i++] = 0;
			a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; a[i++] = 0;
		}
		a[i++] = EGL_NONE;

		EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT,
					      EGL_LINUX_DMA_BUF_EXT, NULL, a);
		const char *label = with_mod ? "with explicit LINEAR modifier"
					     : "without modifier attributes";
		if (img == EGL_NO_IMAGE) {
			printf("FAIL     : eglCreateImage %s -> error=0x%x\n",
			       label, eglGetError());
		} else {
			printf("PASS     : eglCreateImage %s\n", label);
			/* Verify the first import that worked; one readback suffices. */
			if (verify && !import_succeeded)
				verify_failed = verify_import(dpy, img, width, height, pitch, size);
			import_succeeded = 1;
			eglDestroyImage(dpy, img);
		}
		if (!has_mod)
			break;
	}
	eglTerminate(dpy);
	gbm_device_destroy(gbm);
	close(gpu_fd);
	close(dbuf);
	close(udev);
	close(mfd);

	if (!import_succeeded) {
		fprintf(stderr, "FAIL: the GPU rejected every DMA-BUF import variant\n");
		return 1;
	}

	return verify_failed;
}
