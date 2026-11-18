// SPDX-License-Identifier: MIT
/*
 * Scenario driver standing in for kwin_wayland. Each argument is one step;
 * each step prints one line that tests/kwin-bridge/run.sh matches:
 *
 *   open:PATH        DrmDevice::openWithAuthentication on PATH -> slot N
 *   dumb:N           allocateDumb()'s gbm_device_get_fd (called from libkwin)
 *   direct:N         gbm_device_get_fd called from outside libkwin
 *   scanout:N        a scanout buffer from slot N's allocator
 *   close:N          gbm_device_destroy
 *   compat:M:m       GpuManager::compatibleRenderDevice(dev_t)
 *   compatdev:M:m    GpuManager::compatibleRenderDevice(DrmDevice *)
 *   map:M:m=NAME     what KWin's own map says for a device
 *   env              LD_PRELOAD and the bridge's marker after start-up
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct gbm_device;
int gbm_device_get_fd(struct gbm_device *gbm);
uint32_t fake_gbm_last_flags(void);
void kwin_test_map(unsigned long id, const char *name);
struct gbm_device *kwin_test_open(int fd);
int kwin_test_dumb_fd(struct gbm_device *gbm);
int kwin_test_scanout_buffer(struct gbm_device *gbm);
void kwin_test_close(struct gbm_device *gbm);

struct kwin_drm_device {
	dev_t id;
};

const char *compatible_by_id(const void *self, unsigned long id)
	__asm__("_ZNK4KWin10GpuManager22compatibleRenderDeviceEm");
const char *compatible_by_device(const void *self,
				 const struct kwin_drm_device *device)
	__asm__("_ZNK4KWin10GpuManager22compatibleRenderDeviceEPNS_9DrmDeviceE");

static void print_fd(const char *label, int index, int fd)
{
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0) {
		printf("%s %d fd=invalid\n", label, index);
		return;
	}
	printf("%s %d rdev=%u:%u\n", label, index, major(st.st_rdev),
	       minor(st.st_rdev));
}

static unsigned long parse_dev(const char *text)
{
	unsigned int maj = 0, min = 0;

	sscanf(text, "%u:%u", &maj, &min);
	return makedev(maj, min);
}

int main(int argc, char **argv)
{
	struct gbm_device *slots[8] = {0};
	int count = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];

		if (strncmp(step, "open:", 5) == 0) {
			int fd = open(step + 5, O_RDWR | O_CLOEXEC);

			slots[count] = kwin_test_open(fd);
			print_fd("open", count, gbm_device_get_fd(slots[count]));
			count++;
		} else if (strncmp(step, "dumb:", 5) == 0) {
			int n = atoi(step + 5);

			print_fd("dumb", n, kwin_test_dumb_fd(slots[n]));
		} else if (strncmp(step, "direct:", 7) == 0) {
			int n = atoi(step + 7);

			print_fd("direct", n, gbm_device_get_fd(slots[n]));
		} else if (strncmp(step, "scanout:", 8) == 0) {
			int n = atoi(step + 8);
			int ok = kwin_test_scanout_buffer(slots[n]);

			printf("scanout %d ok=%d flags=0x%x\n", n, ok,
			       fake_gbm_last_flags());
		} else if (strncmp(step, "close:", 6) == 0) {
			int n = atoi(step + 6);

			kwin_test_close(slots[n]);
			slots[n] = NULL;
			printf("close %d\n", n);
		} else if (strncmp(step, "compat:", 7) == 0) {
			const char *name = compatible_by_id(NULL, parse_dev(step + 7));

			printf("compat %s -> %s\n", step + 7, name ? name : "none");
		} else if (strncmp(step, "compatdev:", 10) == 0) {
			struct kwin_drm_device device = {.id = parse_dev(step + 10)};
			const char *name = compatible_by_device(NULL, &device);

			printf("compatdev %s -> %s\n", step + 10,
			       name ? name : "none");
		} else if (strncmp(step, "map:", 4) == 0) {
			char *name = strchr(step, '=');

			kwin_test_map(parse_dev(step + 4), name ? name + 1 : "?");
		} else if (strcmp(step, "env") == 0) {
			const char *preload = getenv("LD_PRELOAD");
			const char *marker = getenv("HERMES_KMS_KWIN_BRIDGE_ACTIVE");

			printf("env LD_PRELOAD=%s marker=%s\n",
			       preload ? preload : "(unset)",
			       marker ? marker : "(unset)");
		} else {
			fprintf(stderr, "unknown step %s\n", step);
			return 2;
		}
	}
	return 0;
}
