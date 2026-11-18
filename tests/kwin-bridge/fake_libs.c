// SPDX-License-Identifier: MIT
/*
 * Stand-ins for libgbm, libdrm and libkwin, built as separate shared objects
 * by tests/kwin-bridge/run.sh (FAKE_GBM, FAKE_DRM or FAKE_KWIN selects which).
 * They implement only what the KWin render bridge and the scenario driver
 * call, with character devices from /dev standing in for DRM nodes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(FAKE_GBM)

struct gbm_device {
	int fd;
};
struct gbm_bo;

static uint32_t last_flags;

struct gbm_device *gbm_create_device(int fd)
{
	struct gbm_device *device;

	if (fd < 0) {
		errno = EBADF;
		return NULL;
	}
	device = calloc(1, sizeof(*device));
	if (device)
		device->fd = fd;
	return device;
}

void gbm_device_destroy(struct gbm_device *device)
{
	free(device);
}

int gbm_device_get_fd(struct gbm_device *device)
{
	return device->fd;
}

static struct gbm_bo *allocate(struct gbm_device *device, uint32_t flags)
{
	last_flags = flags;
	/* GBM_BO_USE_SCANOUT is bit 0; emulate a render node refusing it. */
	if ((flags & 1u) && getenv("HKB_FAKE_GBM_NO_SCANOUT"))
		return NULL;
	return (struct gbm_bo *)device;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *device, uint32_t width,
			     uint32_t height, uint32_t format, uint32_t flags)
{
	(void)width;
	(void)height;
	(void)format;
	return allocate(device, flags);
}

struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *device,
					     uint32_t width, uint32_t height,
					     uint32_t format,
					     const uint64_t *modifiers,
					     const unsigned int count,
					     uint32_t flags)
{
	(void)width;
	(void)height;
	(void)format;
	(void)modifiers;
	(void)count;
	return allocate(device, flags);
}

uint32_t fake_gbm_last_flags(void)
{
	return last_flags;
}

#elif defined(FAKE_DRM)

#include <xf86drm.h>

/* HKB_FAKE_GPUS="10de@0000:01:00.0=/dev/zero;1002@0000:03:00.0=/dev/urandom" */
struct fake_gpu {
	unsigned int vendor, domain, bus, dev, func;
	char node[64];
};

static int fake_gpus(struct fake_gpu *gpus, int max)
{
	const char *spec = getenv("HKB_FAKE_GPUS");
	int count = 0;

	while (spec && *spec && count < max) {
		struct fake_gpu *gpu = &gpus[count];
		int used = 0;

		if (sscanf(spec, "%x@%x:%x:%x.%x=%63[^;]%n", &gpu->vendor,
			   &gpu->domain, &gpu->bus, &gpu->dev, &gpu->func,
			   gpu->node, &used) != 6)
			break;
		count++;
		spec += used;
		if (*spec == ';')
			spec++;
	}
	return count;
}

static drmDevicePtr make_device(const struct fake_gpu *gpu)
{
	drmDevicePtr device = calloc(1, sizeof(*device));

	device->bustype = DRM_BUS_PCI;
	device->available_nodes = 1 << DRM_NODE_RENDER;
	device->nodes = calloc(DRM_NODE_MAX, sizeof(char *));
	device->nodes[DRM_NODE_RENDER] = strdup(gpu->node);
	device->businfo.pci = calloc(1, sizeof(*device->businfo.pci));
	device->businfo.pci->domain = (uint16_t)gpu->domain;
	device->businfo.pci->bus = (uint8_t)gpu->bus;
	device->businfo.pci->dev = (uint8_t)gpu->dev;
	device->businfo.pci->func = (uint8_t)gpu->func;
	device->deviceinfo.pci = calloc(1, sizeof(*device->deviceinfo.pci));
	device->deviceinfo.pci->vendor_id = (uint16_t)gpu->vendor;
	return device;
}

void drmFreeDevice(drmDevicePtr *device)
{
	if (!device || !*device)
		return;
	free((*device)->nodes[DRM_NODE_RENDER]);
	free((*device)->nodes);
	free((*device)->businfo.pci);
	free((*device)->deviceinfo.pci);
	free(*device);
	*device = NULL;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; i < count; i++)
		drmFreeDevice(&devices[i]);
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max)
{
	struct fake_gpu gpus[8];
	int count = fake_gpus(gpus, 8);

	(void)flags;
	if (!devices)
		return count;
	if (count > max)
		count = max;
	for (int i = 0; i < count; i++)
		devices[i] = make_device(&gpus[i]);
	return count;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	struct fake_gpu gpus[8];
	int count = fake_gpus(gpus, 8);
	struct stat opened;

	(void)flags;
	if (fstat(fd, &opened) != 0)
		return -errno;
	for (int i = 0; i < count; i++) {
		struct stat node;

		if (stat(gpus[i].node, &node) == 0 &&
		    node.st_rdev == opened.st_rdev) {
			*device = make_device(&gpus[i]);
			return 0;
		}
	}
	return -ENODEV;
}

#elif defined(FAKE_KWIN)

struct gbm_device;
struct gbm_bo;
struct gbm_device *gbm_create_device(int fd);
void gbm_device_destroy(struct gbm_device *gbm);
int gbm_device_get_fd(struct gbm_device *gbm);
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
					     uint32_t width, uint32_t height,
					     uint32_t format,
					     const uint64_t *modifiers,
					     const unsigned int count,
					     uint32_t flags);

struct kwin_drm_device {
	dev_t id;
};

/* GpuManager's dev_t -> RenderDevice map, with names standing in for devices. */
static struct {
	unsigned long id;
	const char *name;
} map[16];

void kwin_test_map(unsigned long id, const char *name)
{
	for (int i = 0; i < 16; i++) {
		if (!map[i].name || map[i].id == id) {
			map[i].id = id;
			map[i].name = name;
			return;
		}
	}
}

void fake_allocator_ctor(void *self, void *device)
	__asm__("_ZN4KWin26GbmGraphicsBufferAllocatorC1EPNS_9DrmDeviceE");
void fake_allocator_ctor(void *self, void *device)
{
	(void)self;
	(void)device;
}

unsigned long fake_device_id(const struct kwin_drm_device *device)
	__asm__("_ZNK4KWin9DrmDevice8deviceIdEv");
unsigned long fake_device_id(const struct kwin_drm_device *device)
{
	return device->id;
}

const char *fake_by_id(const void *self, unsigned long id)
	__asm__("_ZNK4KWin10GpuManager22compatibleRenderDeviceEm");
const char *fake_by_id(const void *self, unsigned long id)
{
	(void)self;
	for (int i = 0; i < 16; i++) {
		if (map[i].name && map[i].id == id)
			return map[i].name;
	}
	return NULL;
}

/* Like KWin, the DrmDevice overload calls the dev_t one; with -fPIC that
 * call goes through the PLT, so the bridge sees it too. */
const char *fake_by_device(const void *self,
			   const struct kwin_drm_device *device)
	__asm__("_ZNK4KWin10GpuManager22compatibleRenderDeviceEPNS_9DrmDeviceE");
const char *fake_by_device(const void *self,
			   const struct kwin_drm_device *device)
{
	return fake_by_id(self, fake_device_id(device));
}

/* DrmDevice::openWithAuthentication */
struct gbm_device *kwin_test_open(int fd)
{
	return gbm_create_device(fd);
}

/* GbmGraphicsBufferAllocator's allocateDumb(), which uses the fd it gets
 * (drmIoctl), so the call is never a tail call out of libkwin. */
int kwin_test_dumb_fd(struct gbm_device *gbm)
{
	int fd = gbm_device_get_fd(gbm);

	__asm__ volatile("" : : : "memory");
	return fd;
}

/* allocateDmaBuf() for a scanout swapchain */
int kwin_test_scanout_buffer(struct gbm_device *gbm)
{
	static const uint64_t modifiers[] = {0x0300000000e08014ull};

	return gbm_bo_create_with_modifiers2(gbm, 1920, 1080, 0x34325258,
					     modifiers, 1, 1u | 4u) != NULL;
}

void kwin_test_close(struct gbm_device *gbm)
{
	gbm_device_destroy(gbm);
}

#else
#error "define FAKE_GBM, FAKE_DRM or FAKE_KWIN"
#endif
