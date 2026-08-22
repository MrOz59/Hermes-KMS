/* hermes_cursor_probe.c - is the compositor using the hermes-kms cursor plane?
 *
 * Polls the hermes-kms DRM planes and reports, for the cursor plane:
 *   FB_ID, CRTC_ID, SRC_X/SRC_Y (16.16 fixed), CRTC_X/CRTC_Y
 * and, for the primary plane: FB_ID (the framebuffer the capture sees).
 *
 * Interpretation:
 *   - cursor plane FB_ID != 0 and changing with pointer motion:
 *       the compositor is using the HW cursor plane. The pointer is NOT
 *       drawn into the primary framebuffer, so the capture stream never
 *       contains the compositor's cursor pixels (the hermes driver stores
 *       nothing from cursor commits; Apollo would have to read the plane).
 *   - cursor plane FB_ID == 0 while moving the pointer:
 *       the compositor is drawing the cursor itself (software cursor) into
 *       the primary framebuffer; cursor pixels ARE in the captured frame.
 *
 * Build: cc $(pkg-config --cflags libdrm) -o hermes_cursor_probe \
 *            hermes_cursor_probe.c $(pkg-config --libs libdrm)
 *
 * Usage: hermes_cursor_probe [--device /dev/dri/cardN] [--interval-ms N]
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

struct plane_info {
	uint32_t id;
	uint32_t type;
	uint32_t prop_fb_id;
	uint32_t prop_crtc_id;
	uint32_t prop_src_x;
	uint32_t prop_src_y;
	uint32_t prop_crtc_x;
	uint32_t prop_crtc_y;
};

static int find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		return 0;
	}
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			int ret = (int)props->props[i];
			drmModeFreeProperty(p);
			drmModeFreeObjectProperties(props);
			return ret;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return 0;
}

static uint64_t get_prop(int fd, uint32_t obj_id, uint32_t obj_type, uint32_t prop_id)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		return 0;
	}
	for (uint32_t i = 0; i < props->count_props; i++) {
		if (props->props[i] == prop_id) {
			uint64_t v = props->prop_values[i];
			drmModeFreeObjectProperties(props);
			return v;
		}
	}
	drmModeFreeObjectProperties(props);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char *dev = "/dev/dri/card0";
	int interval_ms = 250;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			dev = argv[++i];
		} else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc) {
			interval_ms = atoi(argv[++i]);
		} else {
			fprintf(stderr, "usage: %s [--device /dev/dri/cardN] [--interval-ms N]\n", argv[0]);
			return 1;
		}
	}

	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		perror("drmModeGetPlaneResources");
		return 1;
	}

	struct plane_info primary = {0}, cursor = {0};

	for (uint32_t i = 0; i < planes->count_planes; i++) {
		drmModePlanePtr p = drmModeGetPlane(fd, planes->planes[i]);
		if (!p) {
			continue;
		}
		int type_prop = find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type");
		uint64_t type = type_prop ? get_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, (uint32_t)type_prop) : 0;

		if (type == DRM_PLANE_TYPE_PRIMARY && !primary.id) {
			primary.id = p->plane_id;
			primary.type = (uint32_t)type;
			primary.prop_fb_id = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			primary.prop_crtc_id = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
		} else if (type == DRM_PLANE_TYPE_CURSOR && !cursor.id) {
			cursor.id = p->plane_id;
			cursor.type = (uint32_t)type;
			cursor.prop_fb_id = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
			cursor.prop_crtc_id = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
			cursor.prop_src_x = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
			cursor.prop_src_y = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
			cursor.prop_crtc_x = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
			cursor.prop_crtc_y = (uint32_t)find_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);

	printf("device: %s\n", dev);
	printf("primary plane: id=%u fb_id_prop=%u crtc_id_prop=%u\n",
	       primary.id, primary.prop_fb_id, primary.prop_crtc_id);
	printf("cursor plane:  id=%u fb_id_prop=%u crtc_id_prop=%u src_x=%u src_y=%u crtc_x=%u crtc_y=%u\n",
	       cursor.id, cursor.prop_fb_id, cursor.prop_crtc_id,
	       cursor.prop_src_x, cursor.prop_src_y, cursor.prop_crtc_x, cursor.prop_crtc_y);
	if (!cursor.id) {
		printf("NO CURSOR PLANE found on this device.\n");
	}

	uint64_t last_pfb = ~0ULL, last_cfb = ~0ULL, last_crtc = ~0ULL;
	int64_t last_sx = -1, last_sy = -1;
	uint64_t heartbeat = 0;

	for (;;) {
		uint64_t pfb = primary.id ? get_prop(fd, primary.id, DRM_MODE_OBJECT_PLANE, primary.prop_fb_id) : 0;
		uint64_t cfb = cursor.id ? get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_fb_id) : 0;
		uint64_t ccrtc = cursor.id ? get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_crtc_id) : 0;
		int64_t sx = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_src_x) : -1;
		int64_t sy = cursor.id ? (int64_t)(int32_t)get_prop(fd, cursor.id, DRM_MODE_OBJECT_PLANE, cursor.prop_src_y) : -1;

		int changed = (pfb != last_pfb) || (cfb != last_cfb) || (ccrtc != last_crtc) || (sx != last_sx) || (sy != last_sy);
		if (changed) {
			printf("[change] primary_fb=%" PRIu64 " cursor_fb=%" PRIu64 " cursor_crtc=%" PRIu64
			       " cursor_src=(%d.%04d, %d.%04d)\n",
			       pfb, cfb, ccrtc,
			       (int)(sx >> 16), (int)((sx & 0xFFFF) * 10000 / 65536),
			       (int)(sy >> 16), (int)((sy & 0xFFFF) * 10000 / 65536));
			last_pfb = pfb;
			last_cfb = cfb;
			last_crtc = ccrtc;
			last_sx = sx;
			last_sy = sy;
		} else if (++heartbeat % 8 == 0) {
			printf("[same]   primary_fb=%" PRIu64 " cursor_fb=%" PRIu64 " cursor_crtc=%" PRIu64 "\n",
			       pfb, cfb, ccrtc);
		}

		usleep((useconds_t)interval_ms * 1000);
	}

	close(fd);
	return 0;
}
