// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Hermes-KMS contributors
 *
 * The approach (a GBM allocator for a display-only virtual KMS device opened
 * on a real GPU's render node, preloaded into kwin_wayland as a set-user-ID
 * trust-marked library) follows Nonary/libvirtualdisplay's
 * linux/kwin_gpu_interposer.cpp, Copyright (c) 2026 Chase Payne, MIT License.
 * The KWin 6.8 hook points, the device policy and the safety rules below are
 * this project's own; see docs/kwin-render-bridge.md.
 */

/*
 * KWin render bridge for Hermes-KMS.
 *
 * Why: Hermes-KMS has no rendering engine behind its render node, so KWin 6.8
 * finds no hardware RenderDevice for a Hermes-KMS card and falls back to
 * MultiGpuImportMode::DumbBuffer: it renders the virtual output on the real
 * GPU, then copies it with the CPU into a system-memory dumb buffer. NVIDIA
 * cannot import that buffer correctly, so Hermes copies it again.
 *
 * What this library changes, only inside /usr/bin/kwin_wayland and only for
 * Hermes-KMS primary nodes:
 *
 *   1. gbm_create_device() for a Hermes-KMS card returns a GBM device opened
 *      on the selected GPU's render node. KWin keeps the card's own KMS fd for
 *      every DRM ioctl; only buffer allocation moves to the GPU.
 *   2. gbm_device_get_fd() on that GBM device returns (a duplicate of) the KMS
 *      fd when the caller is libkwin: KWin 6.8 allocates dumb buffers (cursor,
 *      QPainter fallback) through it, and CREATE_DUMB must reach the card.
 *   3. KWin::GpuManager::compatibleRenderDevice() answers, for a routed card
 *      (its primary or render node), with the RenderDevice of the selected
 *      GPU. KWin then renders the output straight into GPU buffers allocated
 *      by (1) and scans them out through DrmGpu::importBuffer (PRIME), which
 *      Hermes-KMS re-exports to capture as the GPU's own DMA-BUF.
 *
 * Every other process, device and call passes through unchanged. The library
 * links only libc: libdrm and libgbm are reached through the symbols the
 * compositor already loaded, so a set-user-ID program that preloads it gains
 * no new dependencies and no active hook.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <xf86drm.h> /* types and constants only; nothing from libdrm is linked */

#include "bridge_policy.h"

#define HKB_EXPORT __attribute__((visibility("default")))
#define HKB_MAX_ROUTES 16

#ifndef HKB_KWIN_EXECUTABLE
#define HKB_KWIN_EXECUTABLE "/usr/bin/kwin_wayland"
#endif

/* Opaque GBM types; the prototypes below match <gbm.h>. */
struct gbm_device;
struct gbm_bo;

HKB_EXPORT struct gbm_device *gbm_create_device(int fd);
HKB_EXPORT void gbm_device_destroy(struct gbm_device *gbm);
HKB_EXPORT int gbm_device_get_fd(struct gbm_device *gbm);
HKB_EXPORT struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width,
					uint32_t height, uint32_t format,
					uint32_t flags);
HKB_EXPORT struct gbm_bo *
gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t width,
			      uint32_t height, uint32_t format,
			      const uint64_t *modifiers,
			      const unsigned int count, uint32_t flags);

/* KWin 6.8 C++ entry points, by their Itanium names. dev_t is unsigned long
 * ("m") on every LP64 glibc target KWin ships on. */
#define HKB_SYM_ALLOCATOR_CTOR "_ZN4KWin26GbmGraphicsBufferAllocatorC1EPNS_9DrmDeviceE"
#define HKB_SYM_COMPATIBLE_BY_ID "_ZNK4KWin10GpuManager22compatibleRenderDeviceEm"
#define HKB_SYM_COMPATIBLE_BY_DEVICE "_ZNK4KWin10GpuManager22compatibleRenderDeviceEPNS_9DrmDeviceE"
#define HKB_SYM_DEVICE_ID "_ZNK4KWin9DrmDevice8deviceIdEv"

_Static_assert(sizeof(dev_t) == sizeof(unsigned long),
	       "KWin's compatibleRenderDevice(dev_t) is mangled for an unsigned long dev_t");

HKB_EXPORT void *hkb_compatible_by_id(const void *self, unsigned long id)
	__asm__(HKB_SYM_COMPATIBLE_BY_ID);
HKB_EXPORT void *hkb_compatible_by_device(const void *self, const void *device)
	__asm__(HKB_SYM_COMPATIBLE_BY_DEVICE);

typedef struct gbm_device *(*gbm_create_device_fn)(int);
typedef void (*gbm_device_destroy_fn)(struct gbm_device *);
typedef int (*gbm_device_get_fd_fn)(struct gbm_device *);
typedef struct gbm_bo *(*gbm_bo_create_fn)(struct gbm_device *, uint32_t,
					   uint32_t, uint32_t, uint32_t);
typedef struct gbm_bo *(*gbm_bo_create_with_modifiers2_fn)(
	struct gbm_device *, uint32_t, uint32_t, uint32_t, const uint64_t *,
	const unsigned int, uint32_t);
typedef int (*drm_get_devices2_fn)(uint32_t, drmDevicePtr[], int);
typedef void (*drm_free_devices_fn)(drmDevicePtr[], int);
typedef int (*drm_get_device2_fn)(int, uint32_t, drmDevicePtr *);
typedef void (*drm_free_device_fn)(drmDevicePtr *);
typedef void *(*compatible_by_id_fn)(const void *, unsigned long);
typedef void *(*compatible_by_device_fn)(const void *, const void *);
typedef unsigned long (*device_id_fn)(const void *);

struct hkb_route {
	bool used;
	struct gbm_device *gbm; /* allocator handed to KWin */
	dev_t kms_rdev; /* Hermes-KMS primary node */
	dev_t kms_render_rdev; /* its render node, 0 when absent */
	dev_t gpu_render_rdev; /* selected GPU's render node */
	int gpu_fd; /* owned: the render node GBM was created on */
	int kms_fd; /* owned: duplicate of KWin's KMS fd */
	struct hkb_pci_address gpu;
	char gpu_node[PATH_MAX];
	unsigned long render_redirects;
	bool logged_redirect;
	bool logged_gpu_buffer;
	bool logged_dumb_before_render;
};

static pthread_mutex_t routes_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hkb_route routes[HKB_MAX_ROUTES];
static bool hooks_enabled; /* written once by the constructor */
static bool logged_selection_failure;
static bool logged_second_device;

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */

__attribute__((format(printf, 1, 2))) static void hkb_log(const char *fmt, ...)
{
	char line[512];
	va_list args;
	int prefix, length;

	prefix = snprintf(line, sizeof(line), "hermes-kms-kwin-bridge: ");
	va_start(args, fmt);
	length = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1,
			   fmt, args);
	va_end(args);
	if (length < 0)
		return;
	length += prefix;
	if ((size_t)length > sizeof(line) - 2)
		length = (int)sizeof(line) - 2;
	line[length++] = '\n';
	(void)!write(STDERR_FILENO, line, (size_t)length);
}

/* ------------------------------------------------------------------------ */
/* Symbol resolution                                                        */

static void *resolve(void **slot, void *handle, const char *name)
{
	void *function = __atomic_load_n(slot, __ATOMIC_ACQUIRE);

	if (function)
		return function;
	function = dlsym(handle, name);
	if (function)
		__atomic_store_n(slot, function, __ATOMIC_RELEASE);
	return function;
}

#define REAL(type, handle, name)                                         \
	({                                                               \
		static void *slot_;                                      \
		(type)resolve(&slot_, (handle), (name));                 \
	})

/* ------------------------------------------------------------------------ */
/* sysfs: what a character device is                                        */

static const char *sysfs_root(void)
{
#ifdef HKB_TESTING
	const char *root = getenv("HKB_TEST_SYSFS_ROOT");

	if (root && *root)
		return root;
#endif
	return "/sys";
}

static bool rdev_sysfs_path(dev_t rdev, const char *suffix, char *out,
			    size_t size)
{
	int n = snprintf(out, size, "%s/dev/char/%u:%u%s", sysfs_root(),
			 major(rdev), minor(rdev), suffix);

	return n > 0 && (size_t)n < size;
}

/* Basename of a symlink's target under /sys/dev/char/M:m<suffix>. */
static bool rdev_link_basename(dev_t rdev, const char *suffix, char *out,
			       size_t size)
{
	char path[PATH_MAX], target[PATH_MAX];
	const char *base;
	ssize_t length;

	if (!rdev_sysfs_path(rdev, suffix, path, sizeof(path)))
		return false;
	length = readlink(path, target, sizeof(target) - 1);
	if (length <= 0)
		return false;
	target[length] = '\0';
	base = strrchr(target, '/');
	base = base ? base + 1 : target;
	if (strlen(base) >= size)
		return false;
	strcpy(out, base);
	return true;
}

static bool rdev_is_primary_node(dev_t rdev)
{
	char name[64];

	return rdev_link_basename(rdev, "", name, sizeof(name)) &&
	       strncmp(name, "card", 4) == 0 && name[4] >= '0' &&
	       name[4] <= '9';
}

static bool rdev_is_render_node(dev_t rdev)
{
	char name[64];

	return rdev_link_basename(rdev, "", name, sizeof(name)) &&
	       strncmp(name, "renderD", 7) == 0;
}

static bool rdev_is_hermes_kms(dev_t rdev)
{
	char driver[64];

	return rdev_link_basename(rdev, "/device/driver", driver,
				  sizeof(driver)) &&
	       strcmp(driver, HKB_KMS_DRIVER_NAME) == 0;
}

/* The render node that belongs to the same Hermes-KMS card, if any. */
static dev_t sibling_render_node(dev_t kms_rdev)
{
	char directory[PATH_MAX];
	struct dirent *entry;
	dev_t result = 0;
	DIR *dir;

	if (!rdev_sysfs_path(kms_rdev, "/device/drm", directory,
			     sizeof(directory)))
		return 0;
	dir = opendir(directory);
	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		char path[PATH_MAX], text[32];
		unsigned int maj, min;
		ssize_t length;
		int fd;

		if (strncmp(entry->d_name, "renderD", 7) != 0)
			continue;
		if (snprintf(path, sizeof(path), "%s/%s/dev", directory,
			     entry->d_name) >= (int)sizeof(path))
			continue;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		length = read(fd, text, sizeof(text) - 1);
		close(fd);
		if (length <= 0)
			continue;
		text[length] = '\0';
		if (sscanf(text, "%u:%u", &maj, &min) == 2) {
			result = makedev(maj, min);
			break;
		}
	}
	closedir(dir);
	return result;
}

/* ------------------------------------------------------------------------ */
/* Choosing and opening the render GPU                                      */

struct hkb_gpu {
	struct hkb_pci_address pci;
	char node[PATH_MAX];
};

static bool device_has_render_node(const drmDevicePtr device)
{
	return device && device->bustype == DRM_BUS_PCI &&
	       device->businfo.pci && device->deviceinfo.pci &&
	       (device->available_nodes & (1 << DRM_NODE_RENDER)) &&
	       device->nodes && device->nodes[DRM_NODE_RENDER];
}

static struct hkb_pci_address to_address(const drmPciBusInfo *info)
{
	struct hkb_pci_address address = {
		.domain = info->domain,
		.bus = info->bus,
		.dev = info->dev,
		.func = info->func,
	};

	return address;
}

/*
 * HERMES_KMS_KWIN_RENDER_PCI picks one GPU of any vendor; without it the
 * NVIDIA GPU with the lowest PCI address is used. No NVIDIA GPU, no routing:
 * AMD and Intel already import Hermes-KMS dumb buffers on the capture side,
 * and moving their rendering is untested, so it stays an explicit choice.
 */
static bool select_gpu(struct hkb_gpu *out)
{
	drm_get_devices2_fn get_devices =
		REAL(drm_get_devices2_fn, RTLD_DEFAULT, "drmGetDevices2");
	drm_free_devices_fn free_devices =
		REAL(drm_free_devices_fn, RTLD_DEFAULT, "drmFreeDevices");
	const char *configured_text = getenv("HERMES_KMS_KWIN_RENDER_PCI");
	struct hkb_pci_address configured;
	bool have_configured = false, found = false;
	drmDevicePtr devices[64];
	int count;

	if (configured_text && *configured_text) {
		if (!hkb_parse_pci(configured_text, &configured)) {
			if (!logged_selection_failure)
				hkb_log("HERMES_KMS_KWIN_RENDER_PCI=%s is not a PCI address (DDDD:BB:DD.F); not routing",
					configured_text);
			logged_selection_failure = true;
			return false;
		}
		have_configured = true;
	}
	if (!get_devices || !free_devices)
		return false;
	count = get_devices(0, devices, (int)(sizeof(devices) / sizeof(devices[0])));
	if (count <= 0)
		return false;
	for (int i = 0; i < count; i++) {
		struct hkb_pci_address address;

		if (!device_has_render_node(devices[i]))
			continue;
		address = to_address(devices[i]->businfo.pci);
		if (have_configured) {
			if (!hkb_pci_equal(&address, &configured))
				continue;
		} else if (devices[i]->deviceinfo.pci->vendor_id !=
			   HKB_NVIDIA_VENDOR_ID) {
			continue;
		}
		if (found && !hkb_pci_less(&address, &out->pci))
			continue;
		if (strlen(devices[i]->nodes[DRM_NODE_RENDER]) >=
		    sizeof(out->node))
			continue;
		out->pci = address;
		strcpy(out->node, devices[i]->nodes[DRM_NODE_RENDER]);
		found = true;
	}
	free_devices(devices, count);
	if (!found && !logged_selection_failure) {
		if (have_configured)
			hkb_log("no render node at HERMES_KMS_KWIN_RENDER_PCI=%s; not routing",
				configured_text);
		else
			hkb_log("no NVIDIA render node found; Hermes-KMS cards keep KWin's native allocator");
		logged_selection_failure = true;
	}
	return found;
}

/* Opens the chosen node and proves it is still that GPU's render node. */
static int open_gpu(const struct hkb_gpu *gpu, dev_t *rdev)
{
	drm_get_device2_fn get_device =
		REAL(drm_get_device2_fn, RTLD_DEFAULT, "drmGetDevice2");
	drm_free_device_fn free_device =
		REAL(drm_free_device_fn, RTLD_DEFAULT, "drmFreeDevice");
	drmDevicePtr opened = NULL;
	struct stat st;
	bool matches = false;
	int fd;

	if (!get_device || !free_device)
		return -1;
	fd = open(gpu->node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
	    rdev_is_render_node(st.st_rdev) &&
	    get_device(fd, 0, &opened) == 0 && device_has_render_node(opened)) {
		struct hkb_pci_address address = to_address(opened->businfo.pci);

		matches = hkb_pci_equal(&address, &gpu->pci);
	}
	if (opened)
		free_device(&opened);
	if (!matches) {
		close(fd);
		return -1;
	}
	*rdev = st.st_rdev;
	return fd;
}

/* ------------------------------------------------------------------------ */
/* Route table (callers hold routes_lock)                                   */

static struct hkb_route *route_by_gbm(const struct gbm_device *gbm)
{
	for (size_t i = 0; i < HKB_MAX_ROUTES; i++) {
		if (routes[i].used && routes[i].gbm == gbm)
			return &routes[i];
	}
	return NULL;
}

static struct hkb_route *route_by_rdev(dev_t rdev)
{
	if (rdev == 0)
		return NULL;
	for (size_t i = 0; i < HKB_MAX_ROUTES; i++) {
		if (routes[i].used && (routes[i].kms_rdev == rdev ||
				       routes[i].kms_render_rdev == rdev))
			return &routes[i];
	}
	return NULL;
}

static struct hkb_route *free_route(void)
{
	for (size_t i = 0; i < HKB_MAX_ROUTES; i++) {
		if (!routes[i].used)
			return &routes[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Who is calling                                                           */

static bool caller_is_libkwin(const void *address)
{
	Dl_info info;

	return dladdr(address, &info) && hkb_is_libkwin_name(info.dli_fname);
}

/* ------------------------------------------------------------------------ */
/* GBM hooks                                                                */

HKB_EXPORT struct gbm_device *gbm_create_device(int fd)
{
	gbm_create_device_fn real =
		REAL(gbm_create_device_fn, RTLD_NEXT, "gbm_create_device");
	gbm_device_destroy_fn real_destroy =
		REAL(gbm_device_destroy_fn, RTLD_NEXT, "gbm_device_destroy");
	struct hkb_route *route;
	struct gbm_device *gbm;
	struct hkb_gpu gpu;
	dev_t gpu_rdev = 0;
	struct stat st;
	int saved_errno = errno, gpu_fd, kms_fd;
	bool already_routed;

	if (!real) {
		errno = ENOSYS;
		return NULL;
	}
	if (!__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE))
		return real(fd);
	if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
	    !rdev_is_primary_node(st.st_rdev) || !rdev_is_hermes_kms(st.st_rdev))
		goto pass_through;

	/*
	 * KWin opens a card's allocator once in DrmBackend::addGpu. A second
	 * live GBM device for the same card is DrmGpu's software-rendering
	 * fallback RenderDevice (no udmabuf), which builds an EGL display on
	 * it; that one must stay on the card's own, native GBM.
	 */
	pthread_mutex_lock(&routes_lock);
	already_routed = route_by_rdev(st.st_rdev) != NULL;
	pthread_mutex_unlock(&routes_lock);
	if (already_routed) {
		if (!logged_second_device)
			hkb_log("second GBM device for Hermes-KMS card %u:%u left native (KWin's software-rendering fallback)",
				major(st.st_rdev), minor(st.st_rdev));
		logged_second_device = true;
		goto pass_through;
	}

	if (!select_gpu(&gpu))
		goto pass_through;
	gpu_fd = open_gpu(&gpu, &gpu_rdev);
	if (gpu_fd < 0) {
		hkb_log("could not open %s as the render node of %04x:%02x:%02x.%u; card %u:%u left native",
			gpu.node, gpu.pci.domain, gpu.pci.bus, gpu.pci.dev,
			gpu.pci.func, major(st.st_rdev), minor(st.st_rdev));
		goto pass_through;
	}
	kms_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (kms_fd < 0) {
		close(gpu_fd);
		goto pass_through;
	}
	gbm = real(gpu_fd);
	if (!gbm) {
		hkb_log("GBM refused render node %s; card %u:%u left native",
			gpu.node, major(st.st_rdev), minor(st.st_rdev));
		close(gpu_fd);
		close(kms_fd);
		goto pass_through;
	}

	pthread_mutex_lock(&routes_lock);
	route = route_by_rdev(st.st_rdev) ? NULL : free_route();
	if (route) {
		memset(route, 0, sizeof(*route));
		route->used = true;
		route->gbm = gbm;
		route->kms_rdev = st.st_rdev;
		route->kms_render_rdev = sibling_render_node(st.st_rdev);
		route->gpu_render_rdev = gpu_rdev;
		route->gpu_fd = gpu_fd;
		route->kms_fd = kms_fd;
		route->gpu = gpu.pci;
		strcpy(route->gpu_node, gpu.node);
	}
	pthread_mutex_unlock(&routes_lock);
	if (!route) {
		if (real_destroy)
			real_destroy(gbm);
		close(gpu_fd);
		close(kms_fd);
		goto pass_through;
	}

	hkb_log("Hermes-KMS card %u:%u allocates on %s (%04x:%02x:%02x.%u); KMS stays on the card",
		major(st.st_rdev), minor(st.st_rdev), gpu.node, gpu.pci.domain,
		gpu.pci.bus, gpu.pci.dev, gpu.pci.func);
	errno = saved_errno;
	return gbm;

pass_through:
	errno = saved_errno;
	return real(fd);
}

HKB_EXPORT void gbm_device_destroy(struct gbm_device *gbm)
{
	gbm_device_destroy_fn real =
		REAL(gbm_device_destroy_fn, RTLD_NEXT, "gbm_device_destroy");
	int gpu_fd = -1, kms_fd = -1;

	if (!real) {
		errno = ENOSYS;
		return;
	}
	if (__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE)) {
		struct hkb_route *route;

		pthread_mutex_lock(&routes_lock);
		route = route_by_gbm(gbm);
		if (route) {
			gpu_fd = route->gpu_fd;
			kms_fd = route->kms_fd;
			memset(route, 0, sizeof(*route));
		}
		pthread_mutex_unlock(&routes_lock);
	}
	real(gbm);
	/* GBM never owns the fd it was created on. */
	if (gpu_fd >= 0)
		close(gpu_fd);
	if (kms_fd >= 0)
		close(kms_fd);
}

HKB_EXPORT int gbm_device_get_fd(struct gbm_device *gbm)
{
	gbm_device_get_fd_fn real =
		REAL(gbm_device_get_fd_fn, RTLD_NEXT, "gbm_device_get_fd");

	/*
	 * KWin 6.8's allocateDumb() is the only libkwin caller: dumb buffers
	 * belong on the card. GBM and EGL themselves keep the render fd the
	 * device was created on.
	 */
	if (__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE) &&
	    caller_is_libkwin(__builtin_return_address(0))) {
		struct hkb_route *route;
		bool warn = false;
		int kms_fd = -1;
		dev_t kms_rdev = 0;

		pthread_mutex_lock(&routes_lock);
		route = route_by_gbm(gbm);
		if (route) {
			kms_fd = route->kms_fd;
			kms_rdev = route->kms_rdev;
			if (route->render_redirects == 0 &&
			    !route->logged_dumb_before_render) {
				route->logged_dumb_before_render = true;
				warn = true;
			}
		}
		pthread_mutex_unlock(&routes_lock);
		if (kms_fd >= 0) {
			if (warn)
				hkb_log("KWin allocated a dumb buffer on card %u:%u before asking for its render device through a hooked entry point; this KWin build may resolve compatibleRenderDevice() internally, and the output keeps the CPU copy",
					major(kms_rdev), minor(kms_rdev));
			return kms_fd;
		}
	}
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return real(gbm);
}

#define HKB_GBM_BO_USE_SCANOUT (1u << 0) /* GBM_BO_USE_SCANOUT in <gbm.h> */

/* Whether gbm is a routed allocator; logs its first GPU buffer once. */
static bool note_gpu_buffer(struct gbm_device *gbm, uint32_t width,
			    uint32_t height, uint32_t format,
			    unsigned int modifier_count, uint32_t flags)
{
	struct hkb_route *route;
	bool log = false, routed = false;
	dev_t kms_rdev = 0;

	if (!__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE))
		return false;
	pthread_mutex_lock(&routes_lock);
	route = route_by_gbm(gbm);
	if (route) {
		routed = true;
		kms_rdev = route->kms_rdev;
		if (!route->logged_gpu_buffer) {
			route->logged_gpu_buffer = true;
			log = true;
		}
	}
	pthread_mutex_unlock(&routes_lock);
	if (log)
		hkb_log("first GPU buffer for card %u:%u: %ux%u format 0x%08x, %u candidate modifier(s), flags 0x%x",
			major(kms_rdev), minor(kms_rdev), width, height, format,
			modifier_count, flags);
	return routed;
}

static void note_scanout_retry(struct gbm_device *gbm, bool succeeded)
{
	static bool logged;

	if (logged)
		return;
	logged = true;
	(void)gbm;
	hkb_log("the GPU refused GBM_BO_USE_SCANOUT on its render node; %s without it (Hermes-KMS imports any GPU buffer)",
		succeeded ? "allocated" : "also failed");
}

/*
 * A routed allocator lives on a render node, and Hermes-KMS never scans out
 * physically: it only re-exports the imported buffer to capture. If the GPU
 * refuses the scanout usage there, the same buffer without it is what the
 * capture path was validated with.
 */
HKB_EXPORT struct gbm_bo *gbm_bo_create(struct gbm_device *gbm, uint32_t width,
					uint32_t height, uint32_t format,
					uint32_t flags)
{
	gbm_bo_create_fn real = REAL(gbm_bo_create_fn, RTLD_NEXT, "gbm_bo_create");
	struct gbm_bo *bo;
	bool routed;

	if (!real) {
		errno = ENOSYS;
		return NULL;
	}
	routed = note_gpu_buffer(gbm, width, height, format, 0, flags);
	bo = real(gbm, width, height, format, flags);
	if (!bo && routed && (flags & HKB_GBM_BO_USE_SCANOUT)) {
		bo = real(gbm, width, height, format,
			  flags & ~HKB_GBM_BO_USE_SCANOUT);
		note_scanout_retry(gbm, bo != NULL);
	}
	return bo;
}

HKB_EXPORT struct gbm_bo *
gbm_bo_create_with_modifiers2(struct gbm_device *gbm, uint32_t width,
			      uint32_t height, uint32_t format,
			      const uint64_t *modifiers,
			      const unsigned int count, uint32_t flags)
{
	gbm_bo_create_with_modifiers2_fn real =
		REAL(gbm_bo_create_with_modifiers2_fn, RTLD_NEXT,
		     "gbm_bo_create_with_modifiers2");
	struct gbm_bo *bo;
	bool routed;

	if (!real) {
		errno = ENOSYS;
		return NULL;
	}
	routed = note_gpu_buffer(gbm, width, height, format, count, flags);
	bo = real(gbm, width, height, format, modifiers, count, flags);
	if (!bo && routed && (flags & HKB_GBM_BO_USE_SCANOUT)) {
		bo = real(gbm, width, height, format, modifiers, count,
			  flags & ~HKB_GBM_BO_USE_SCANOUT);
		note_scanout_retry(gbm, bo != NULL);
	}
	return bo;
}

/* ------------------------------------------------------------------------ */
/* KWin::GpuManager::compatibleRenderDevice                                 */

/* The render node to ask KWin about instead of id, if id is a routed card. */
static bool redirect_target(dev_t id, dev_t *target)
{
	struct hkb_route *route;
	struct hkb_pci_address gpu;
	bool log = false;
	dev_t kms_rdev = 0;

	pthread_mutex_lock(&routes_lock);
	route = route_by_rdev(id);
	if (route) {
		*target = route->gpu_render_rdev;
		route->render_redirects++;
		if (!route->logged_redirect) {
			route->logged_redirect = true;
			log = true;
			gpu = route->gpu;
			kms_rdev = route->kms_rdev;
		}
	}
	pthread_mutex_unlock(&routes_lock);
	if (log)
		hkb_log("KWin renders Hermes-KMS card %u:%u on %04x:%02x:%02x.%u",
			major(kms_rdev), minor(kms_rdev), gpu.domain, gpu.bus,
			gpu.dev, gpu.func);
	return route != NULL;
}

HKB_EXPORT void *hkb_compatible_by_id(const void *self, unsigned long id)
{
	compatible_by_id_fn real =
		REAL(compatible_by_id_fn, RTLD_NEXT, HKB_SYM_COMPATIBLE_BY_ID);
	dev_t target;

	if (!real)
		return NULL;
	if (__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE) &&
	    redirect_target((dev_t)id, &target)) {
		void *device = real(self, (unsigned long)target);

		if (device)
			return device;
	}
	return real(self, id);
}

HKB_EXPORT void *hkb_compatible_by_device(const void *self, const void *device)
{
	compatible_by_device_fn real = REAL(compatible_by_device_fn, RTLD_NEXT,
					    HKB_SYM_COMPATIBLE_BY_DEVICE);

	if (!real)
		return NULL;
	if (__atomic_load_n(&hooks_enabled, __ATOMIC_ACQUIRE) && device) {
		device_id_fn device_id =
			REAL(device_id_fn, RTLD_DEFAULT, HKB_SYM_DEVICE_ID);
		compatible_by_id_fn by_id = REAL(compatible_by_id_fn, RTLD_NEXT,
						 HKB_SYM_COMPATIBLE_BY_ID);
		dev_t target;

		if (device_id && by_id &&
		    redirect_target((dev_t)device_id(device), &target)) {
			void *render = by_id(self, (unsigned long)target);

			if (render)
				return render;
		}
	}
	return real(self, device);
}

/* ------------------------------------------------------------------------ */
/* Enabling                                                                 */

static bool running_inside_kwin(void)
{
	const char *expected = HKB_KWIN_EXECUTABLE;
	char executable[PATH_MAX];
	ssize_t length;

#ifdef HKB_TESTING
	if (getenv("HKB_TEST_KWIN_EXE"))
		expected = getenv("HKB_TEST_KWIN_EXE");
#endif
	length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
	if (length <= 0)
		return false;
	executable[length] = '\0';
	return strcmp(executable, expected) == 0;
}

static void forget_preload(void)
{
	const char *value = getenv("LD_PRELOAD");
	char rest[4096];

	unsetenv("HERMES_KMS_KWIN_BRIDGE_ACTIVE");
	if (!value)
		return;
	/* Keep anything else the session preloads; drop only this library,
	 * so Xwayland and applications started by KWin never inherit it. */
	if (hkb_strip_preload(value, rest, sizeof(rest)) && rest[0])
		setenv("LD_PRELOAD", rest, 1);
	else
		unsetenv("LD_PRELOAD");
}

/* Every KWin entry point the bridge relies on, all in one reviewed libkwin. */
static bool kwin_supported(enum hkb_mode mode)
{
	void *by_id = dlsym(RTLD_NEXT, HKB_SYM_COMPATIBLE_BY_ID);
	void *by_device = dlsym(RTLD_NEXT, HKB_SYM_COMPATIBLE_BY_DEVICE);
	void *device_id = dlsym(RTLD_DEFAULT, HKB_SYM_DEVICE_ID);
	void *allocator = dlsym(RTLD_DEFAULT, HKB_SYM_ALLOCATOR_CTOR);
	char resolved[PATH_MAX];
	const char *base;
	unsigned int major_version, minor_version;
	Dl_info info, other;

	if (!by_id || !by_device || !device_id || !allocator) {
		hkb_log("this KWin lacks the 6.8 allocator/render-device entry points (%s%s%s%s); bridge disabled",
			by_id ? "" : "compatibleRenderDevice(dev_t) ",
			by_device ? "" : "compatibleRenderDevice(DrmDevice*) ",
			device_id ? "" : "DrmDevice::deviceId ",
			allocator ? "" : "GbmGraphicsBufferAllocator(DrmDevice*)");
		return false;
	}
	if (!dladdr(by_id, &info) || !info.dli_fname ||
	    !realpath(info.dli_fname, resolved)) {
		hkb_log("cannot locate the library that defines KWin's GpuManager; bridge disabled");
		return false;
	}
	if (!dladdr(by_device, &other) || other.dli_fbase != info.dli_fbase ||
	    !dladdr(device_id, &other) || other.dli_fbase != info.dli_fbase ||
	    !dladdr(allocator, &other) || other.dli_fbase != info.dli_fbase) {
		hkb_log("KWin's entry points come from different libraries; bridge disabled");
		return false;
	}
	base = strrchr(resolved, '/');
	base = base ? base + 1 : resolved;
	if (!hkb_parse_kwin_library(base, &major_version, &minor_version)) {
		hkb_log("unrecognised KWin library %s; bridge disabled", base);
		return false;
	}
	if (!hkb_kwin_release_reviewed(major_version, minor_version)) {
		if (mode != HKB_MODE_FORCE) {
			hkb_log("KWin %u.%u has not been reviewed for this bridge (6.8 has); set HERMES_KMS_KWIN_BRIDGE=force to try it",
				major_version, minor_version);
			return false;
		}
		hkb_log("KWin %u.%u is unreviewed; continuing because HERMES_KMS_KWIN_BRIDGE=force",
			major_version, minor_version);
	}
	hkb_log("active in kwin_wayland (%s)", base);
	return true;
}

__attribute__((constructor)) static void hkb_init(void)
{
	const char *active = getenv("HERMES_KMS_KWIN_BRIDGE_ACTIVE");
	const char *mode_text = getenv("HERMES_KMS_KWIN_BRIDGE");
	enum hkb_mode mode;

	/* Loaded anywhere but the compositor (kwin_wayland_wrapper, a
	 * set-user-ID program, ...): stay a pure pass-through. */
	if (!active || strcmp(active, "1") != 0 || !running_inside_kwin())
		return;
	forget_preload();

	mode = hkb_parse_mode(mode_text);
	if (mode == HKB_MODE_INVALID) {
		hkb_log("HERMES_KMS_KWIN_BRIDGE=%s is not auto, off or force; bridge disabled",
			mode_text);
		return;
	}
	if (mode == HKB_MODE_OFF) {
		hkb_log("disabled by HERMES_KMS_KWIN_BRIDGE");
		return;
	}
	if (kwin_supported(mode))
		__atomic_store_n(&hooks_enabled, true, __ATOMIC_RELEASE);
}

/*
 * The installed file carries the set-user-ID bit only as glibc's trust marker
 * for preloading into a capability-bearing kwin_wayland. Executing it directly
 * must do nothing: this entry point exits before any libc code runs.
 */
#ifndef HKB_TESTING
__attribute__((used, noreturn, visibility("hidden"))) void
hkb_exec_entry(void)
{
#if defined(__x86_64__)
	__asm__ volatile("mov $60, %eax\n\tmov $1, %edi\n\tsyscall");
#elif defined(__aarch64__)
	__asm__ volatile("mov x0, #1\n\tmov x8, #93\n\tsvc #0");
#endif
	__builtin_trap();
}
#endif
