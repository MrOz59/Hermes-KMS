// SPDX-License-Identifier: GPL-2.0
/*
 * Hermes-KMS - experimental DRM/KMS virtual display driver for Hermes.
 *
 * This module is intentionally not an EVDI wrapper.  The first milestone is a
 * safe DRM device/connector skeleton; DMA-BUF export and low-latency capture
 * will be built on top of this once the KMS object lifecycle is stable.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>

#include <drm/hermes_kms_drm.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define HERMES_KMS_DRIVER_NAME "hermes-kms"
#define HERMES_KMS_DRIVER_DESC "Hermes virtual KMS display"
#define HERMES_KMS_DRIVER_DATE "20260625"
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 1
#define HERMES_KMS_DRIVER_PATCH 0

#define HERMES_KMS_MIN_WIDTH 640
#define HERMES_KMS_MIN_HEIGHT 480
#define HERMES_KMS_MAX_WIDTH 3840
#define HERMES_KMS_MAX_HEIGHT 2160
#define HERMES_KMS_DEFAULT_WIDTH 1920
#define HERMES_KMS_DEFAULT_HEIGHT 1080
#define HERMES_KMS_DEFAULT_REFRESH_HZ 60
#define HERMES_KMS_MAX_REFRESH_HZ 240

static bool initial_enabled = true;
static unsigned int initial_width = HERMES_KMS_DEFAULT_WIDTH;
static unsigned int initial_height = HERMES_KMS_DEFAULT_HEIGHT;
static unsigned int initial_refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;

module_param(initial_enabled, bool, 0644);
MODULE_PARM_DESC(initial_enabled, "Initial virtual output state");
module_param(initial_width, uint, 0644);
MODULE_PARM_DESC(initial_width, "Initial virtual output width");
module_param(initial_height, uint, 0644);
MODULE_PARM_DESC(initial_height, "Initial virtual output height");
module_param(initial_refresh_hz, uint, 0644);
MODULE_PARM_DESC(initial_refresh_hz, "Initial virtual output refresh rate");

struct hermes_kms_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct mutex state_lock;
	bool output_enabled;
	u32 requested_width;
	u32 requested_height;
	u32 requested_refresh_hz;
	u64 frame_sequence;
	u64 last_update_ns;
	u64 last_enable_ns;
	u64 last_disable_ns;
	u32 framebuffer_id;
	u32 framebuffer_width;
	u32 framebuffer_height;
	u32 framebuffer_format;
	u32 framebuffer_plane_count;
	u32 framebuffer_pitch[4];
	u32 framebuffer_offset[4];
	u64 framebuffer_modifier;
};

static inline struct hermes_kms_device *to_hermes_kms(struct drm_device *drm)
{
	return container_of(drm, struct hermes_kms_device, drm);
}

static const u32 hermes_kms_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static void hermes_kms_clear_frame_locked(struct hermes_kms_device *hdev)
{
	hdev->framebuffer_id = 0;
	hdev->framebuffer_width = 0;
	hdev->framebuffer_height = 0;
	hdev->framebuffer_format = 0;
	hdev->framebuffer_plane_count = 0;
	memset(hdev->framebuffer_pitch, 0, sizeof(hdev->framebuffer_pitch));
	memset(hdev->framebuffer_offset, 0, sizeof(hdev->framebuffer_offset));
	hdev->framebuffer_modifier = 0;
}

static void hermes_kms_track_frame(struct hermes_kms_device *hdev,
				   struct drm_framebuffer *fb)
{
	unsigned int i;
	unsigned int plane_count = 0;

	mutex_lock(&hdev->state_lock);
	hdev->frame_sequence++;
	hdev->last_update_ns = ktime_get_ns();

	if (!fb) {
		hermes_kms_clear_frame_locked(hdev);
		mutex_unlock(&hdev->state_lock);
		return;
	}

	hdev->framebuffer_id = fb->base.id;
	hdev->framebuffer_width = fb->width;
	hdev->framebuffer_height = fb->height;
	hdev->framebuffer_format = fb->format->format;
	hdev->framebuffer_modifier = fb->modifier;

	if (fb->format->num_planes > ARRAY_SIZE(hdev->framebuffer_pitch))
		plane_count = ARRAY_SIZE(hdev->framebuffer_pitch);
	else
		plane_count = fb->format->num_planes;

	hdev->framebuffer_plane_count = plane_count;
	memset(hdev->framebuffer_pitch, 0, sizeof(hdev->framebuffer_pitch));
	memset(hdev->framebuffer_offset, 0, sizeof(hdev->framebuffer_offset));

	for (i = 0; i < plane_count; i++) {
		hdev->framebuffer_pitch[i] = fb->pitches[i];
		hdev->framebuffer_offset[i] = fb->offsets[i];
	}

	mutex_unlock(&hdev->state_lock);
}

static enum drm_connector_status
hermes_kms_connector_detect(struct drm_connector *connector, bool force)
{
	struct hermes_kms_device *hdev = to_hermes_kms(connector->dev);
	enum drm_connector_status status;

	mutex_lock(&hdev->state_lock);
	status = hdev->output_enabled ? connector_status_connected :
					connector_status_disconnected;
	mutex_unlock(&hdev->state_lock);

	return status;
}

static int hermes_kms_connector_get_modes(struct drm_connector *connector)
{
	int count;
	struct hermes_kms_device *hdev = to_hermes_kms(connector->dev);
	u32 preferred_width;
	u32 preferred_height;

	mutex_lock(&hdev->state_lock);
	preferred_width = hdev->requested_width;
	preferred_height = hdev->requested_height;
	mutex_unlock(&hdev->state_lock);

	count = drm_add_modes_noedid(connector, HERMES_KMS_MAX_WIDTH,
				     HERMES_KMS_MAX_HEIGHT);
	drm_set_preferred_mode(connector, preferred_width, preferred_height);

	return count;
}

static const struct drm_connector_helper_funcs hermes_kms_connector_helper_funcs = {
	.get_modes = hermes_kms_connector_get_modes,
};

static const struct drm_connector_funcs hermes_kms_connector_funcs = {
	.detect = hermes_kms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status
hermes_kms_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			   const struct drm_display_mode *mode)
{
	if (mode->hdisplay < HERMES_KMS_MIN_WIDTH ||
	    mode->vdisplay < HERMES_KMS_MIN_HEIGHT)
		return MODE_BAD;

	if (mode->hdisplay > HERMES_KMS_MAX_WIDTH ||
	    mode->vdisplay > HERMES_KMS_MAX_HEIGHT)
		return MODE_VIRTUAL_X;

	if (drm_mode_vrefresh(mode) > HERMES_KMS_MAX_REFRESH_HZ)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static int hermes_kms_pipe_check(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *plane_state,
				 struct drm_crtc_state *crtc_state)
{
	if (!crtc_state->enable)
		return 0;

	crtc_state->no_vblank = true;
	return 0;
}

static void hermes_kms_pipe_enable(struct drm_simple_display_pipe *pipe,
				   struct drm_crtc_state *crtc_state,
				   struct drm_plane_state *plane_state)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct hermes_kms_device *hdev = to_hermes_kms(drm);

	mutex_lock(&hdev->state_lock);
	hdev->last_enable_ns = ktime_get_ns();
	mutex_unlock(&hdev->state_lock);

	hermes_kms_track_frame(hdev, plane_state ? plane_state->fb : NULL);

	drm_info(drm, "enabled virtual display %ux%u@%d\n",
		 crtc_state->mode.hdisplay,
		 crtc_state->mode.vdisplay,
		 drm_mode_vrefresh(&crtc_state->mode));
}

static void hermes_kms_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct hermes_kms_device *hdev = to_hermes_kms(pipe->crtc.dev);

	mutex_lock(&hdev->state_lock);
	hdev->last_disable_ns = ktime_get_ns();
	hermes_kms_clear_frame_locked(hdev);
	mutex_unlock(&hdev->state_lock);

	drm_info(pipe->crtc.dev, "disabled virtual display\n");
}

static void hermes_kms_pipe_update(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct hermes_kms_device *hdev = to_hermes_kms(crtc->dev);
	struct drm_plane_state *plane_state = pipe->plane.state;

	hermes_kms_track_frame(hdev, plane_state ? plane_state->fb : NULL);

	if (event) {
		crtc->state->event = NULL;
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_simple_display_pipe_funcs hermes_kms_pipe_funcs = {
	.mode_valid = hermes_kms_pipe_mode_valid,
	.check = hermes_kms_pipe_check,
	.enable = hermes_kms_pipe_enable,
	.disable = hermes_kms_pipe_disable,
	.update = hermes_kms_pipe_update,
};

static const struct drm_mode_config_funcs hermes_kms_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int hermes_kms_ioctl_get_version(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct drm_hermes_kms_version *version = data;

	memset(version, 0, sizeof(*version));
	version->uapi_version = HERMES_KMS_UAPI_VERSION;
	version->driver_major = HERMES_KMS_DRIVER_MAJOR;
	version->driver_minor = HERMES_KMS_DRIVER_MINOR;
	version->driver_patch = HERMES_KMS_DRIVER_PATCH;
	strscpy(version->driver_name, HERMES_KMS_DRIVER_NAME,
		sizeof(version->driver_name));

	return 0;
}

static int hermes_kms_ioctl_get_caps(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct drm_hermes_kms_caps *caps = data;

	memset(caps, 0, sizeof(*caps));
	caps->flags = HERMES_KMS_CAP_VIRTUAL_OUTPUT |
		      HERMES_KMS_CAP_OUTPUT_CONTROL |
		      HERMES_KMS_CAP_DUMB_BUFFERS |
		      HERMES_KMS_CAP_PRIME_IMPORT |
		      HERMES_KMS_CAP_FRAME_METADATA |
		      HERMES_KMS_CAP_FRAME_ACQUIRE |
		      HERMES_KMS_CAP_DMABUF_EXPORT_PLANNED |
		      HERMES_KMS_CAP_ZERO_COPY_TARGET;
	caps->min_width = HERMES_KMS_MIN_WIDTH;
	caps->min_height = HERMES_KMS_MIN_HEIGHT;
	caps->max_width = HERMES_KMS_MAX_WIDTH;
	caps->max_height = HERMES_KMS_MAX_HEIGHT;
	caps->preferred_width = HERMES_KMS_DEFAULT_WIDTH;
	caps->preferred_height = HERMES_KMS_DEFAULT_HEIGHT;
	caps->max_refresh_hz = HERMES_KMS_MAX_REFRESH_HZ;

	return 0;
}

static int hermes_kms_ioctl_get_status(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_status *status = data;
	struct drm_crtc_state *crtc_state;

	memset(status, 0, sizeof(*status));

	mutex_lock(&hdev->state_lock);
	if (hdev->output_enabled)
		status->flags |= HERMES_KMS_STATUS_OUTPUT_ENABLED |
				 HERMES_KMS_STATUS_CONNECTED;

	status->requested_width = hdev->requested_width;
	status->requested_height = hdev->requested_height;
	status->requested_refresh_hz = hdev->requested_refresh_hz;
	status->frame_sequence = hdev->frame_sequence;
	status->last_update_ns = hdev->last_update_ns;
	status->last_enable_ns = hdev->last_enable_ns;
	status->last_disable_ns = hdev->last_disable_ns;
	status->framebuffer_id = hdev->framebuffer_id;
	status->framebuffer_width = hdev->framebuffer_width;
	status->framebuffer_height = hdev->framebuffer_height;
	status->framebuffer_format = hdev->framebuffer_format;
	status->framebuffer_plane_count = hdev->framebuffer_plane_count;
	memcpy(status->framebuffer_pitch, hdev->framebuffer_pitch,
	       sizeof(status->framebuffer_pitch));
	memcpy(status->framebuffer_offset, hdev->framebuffer_offset,
	       sizeof(status->framebuffer_offset));
	status->framebuffer_modifier = hdev->framebuffer_modifier;
	if (hdev->framebuffer_id)
		status->flags |= HERMES_KMS_STATUS_FRAME_VALID;
	mutex_unlock(&hdev->state_lock);

	status->connector_id = hdev->connector.base.id;
	status->crtc_id = hdev->pipe.crtc.base.id;
	status->plane_id = hdev->pipe.plane.base.id;
	status->encoder_id = hdev->pipe.encoder.base.id;

	crtc_state = hdev->pipe.crtc.state;
	if (crtc_state && crtc_state->enable) {
		status->flags |= HERMES_KMS_STATUS_SCANOUT_ACTIVE;
		status->active_width = crtc_state->mode.hdisplay;
		status->active_height = crtc_state->mode.vdisplay;
		status->active_refresh_hz = drm_mode_vrefresh(&crtc_state->mode);
	}

	return 0;
}

static void hermes_kms_init_invalid_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frame->dma_buf_fd); i++)
		frame->dma_buf_fd[i] = -1;

	frame->sync_file_fd = -1;
}

static int hermes_kms_ioctl_acquire_frame(struct drm_device *drm, void *data,
					  struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_acquire_frame *frame = data;
	u64 requested_flags = frame->flags;

	memset(frame, 0, sizeof(*frame));
	hermes_kms_init_invalid_frame_fds(frame);

	mutex_lock(&hdev->state_lock);
	if (!hdev->framebuffer_id) {
		mutex_unlock(&hdev->state_lock);
		return -ENODATA;
	}

	frame->flags = HERMES_KMS_FRAME_METADATA_VALID |
		       HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED;
	frame->sequence = hdev->frame_sequence;
	frame->timestamp_ns = hdev->last_update_ns;
	frame->modifier = hdev->framebuffer_modifier;
	frame->framebuffer_id = hdev->framebuffer_id;
	frame->width = hdev->framebuffer_width;
	frame->height = hdev->framebuffer_height;
	frame->format = hdev->framebuffer_format;
	frame->plane_count = hdev->framebuffer_plane_count;
	memcpy(frame->pitch, hdev->framebuffer_pitch, sizeof(frame->pitch));
	memcpy(frame->offset, hdev->framebuffer_offset, sizeof(frame->offset));
	mutex_unlock(&hdev->state_lock);

	if (requested_flags & HERMES_KMS_FRAME_REQUEST_DMABUF)
		return -EOPNOTSUPP;

	return 0;
}

static bool hermes_kms_valid_requested_mode(u32 width, u32 height,
					    u32 refresh_hz)
{
	return width >= HERMES_KMS_MIN_WIDTH &&
	       height >= HERMES_KMS_MIN_HEIGHT &&
	       width <= HERMES_KMS_MAX_WIDTH &&
	       height <= HERMES_KMS_MAX_HEIGHT &&
	       refresh_hz > 0 &&
	       refresh_hz <= HERMES_KMS_MAX_REFRESH_HZ;
}

static void hermes_kms_init_output_state(struct drm_device *drm,
					 struct hermes_kms_device *hdev)
{
	u32 width = initial_width;
	u32 height = initial_height;
	u32 refresh_hz = initial_refresh_hz;

	if (!hermes_kms_valid_requested_mode(width, height, refresh_hz)) {
		drm_warn(drm,
			 "invalid initial mode %ux%u@%u, falling back to %ux%u@%u\n",
			 width, height, refresh_hz,
			 HERMES_KMS_DEFAULT_WIDTH,
			 HERMES_KMS_DEFAULT_HEIGHT,
			 HERMES_KMS_DEFAULT_REFRESH_HZ);
		width = HERMES_KMS_DEFAULT_WIDTH;
		height = HERMES_KMS_DEFAULT_HEIGHT;
		refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;
	}

	hdev->output_enabled = initial_enabled;
	hdev->requested_width = width;
	hdev->requested_height = height;
	hdev->requested_refresh_hz = refresh_hz;
}

static int hermes_kms_ioctl_set_output(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_set_output *request = data;
	u32 width = request->width;
	u32 height = request->height;
	u32 refresh_hz = request->refresh_hz;

	if (!request->enabled) {
		mutex_lock(&hdev->state_lock);
		hdev->output_enabled = false;
		mutex_unlock(&hdev->state_lock);
		drm_kms_helper_hotplug_event(drm);
		return 0;
	}

	if (!width)
		width = HERMES_KMS_DEFAULT_WIDTH;
	if (!height)
		height = HERMES_KMS_DEFAULT_HEIGHT;
	if (!refresh_hz)
		refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;

	if (!hermes_kms_valid_requested_mode(width, height, refresh_hz))
		return -EINVAL;

	mutex_lock(&hdev->state_lock);
	hdev->output_enabled = true;
	hdev->requested_width = width;
	hdev->requested_height = height;
	hdev->requested_refresh_hz = refresh_hz;
	mutex_unlock(&hdev->state_lock);

	drm_kms_helper_hotplug_event(drm);
	drm_info(drm, "requested virtual output %ux%u@%u\n",
		 width, height, refresh_hz);

	return 0;
}

static const struct drm_ioctl_desc hermes_kms_ioctls[] = {
	DRM_IOCTL_DEF_DRV(HERMES_KMS_GET_VERSION,
			  hermes_kms_ioctl_get_version,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_GET_CAPS,
			  hermes_kms_ioctl_get_caps,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_GET_STATUS,
			  hermes_kms_ioctl_get_status,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_SET_OUTPUT,
			  hermes_kms_ioctl_set_output,
			  DRM_AUTH | DRM_MASTER),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_ACQUIRE_FRAME,
			  hermes_kms_ioctl_acquire_frame,
			  DRM_RENDER_ALLOW),
};

static const struct file_operations hermes_kms_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
};

static const struct drm_driver hermes_kms_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = HERMES_KMS_DRIVER_NAME,
	.desc = HERMES_KMS_DRIVER_DESC,
	.major = HERMES_KMS_DRIVER_MAJOR,
	.minor = HERMES_KMS_DRIVER_MINOR,
	.fops = &hermes_kms_fops,
	.ioctls = hermes_kms_ioctls,
	.num_ioctls = ARRAY_SIZE(hermes_kms_ioctls),
	DRM_GEM_DMA_DRIVER_OPS,
};

static int hermes_kms_modeset_init(struct hermes_kms_device *hdev)
{
	struct drm_device *drm = &hdev->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = HERMES_KMS_MIN_WIDTH;
	drm->mode_config.min_height = HERMES_KMS_MIN_HEIGHT;
	drm->mode_config.max_width = HERMES_KMS_MAX_WIDTH;
	drm->mode_config.max_height = HERMES_KMS_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &hermes_kms_mode_config_funcs;

	ret = drm_connector_init(drm, &hdev->connector,
				 &hermes_kms_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	drm_connector_helper_add(&hdev->connector,
				 &hermes_kms_connector_helper_funcs);

	hdev->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				 DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_simple_display_pipe_init(drm, &hdev->pipe,
					   &hermes_kms_pipe_funcs,
					   hermes_kms_formats,
					   ARRAY_SIZE(hermes_kms_formats),
					   NULL,
					   &hdev->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

static int hermes_kms_probe(struct platform_device *pdev)
{
	struct hermes_kms_device *hdev;
	struct drm_device *drm;
	int ret;

	hdev = devm_drm_dev_alloc(&pdev->dev, &hermes_kms_driver,
				  struct hermes_kms_device, drm);
	if (IS_ERR(hdev))
		return PTR_ERR(hdev);

	drm = &hdev->drm;
	platform_set_drvdata(pdev, hdev);
	mutex_init(&hdev->state_lock);
	hermes_kms_init_output_state(drm, hdev);

	ret = hermes_kms_modeset_init(hdev);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_info(drm, "registered Hermes-KMS virtual DRM device\n");
	return 0;
}

static void hermes_kms_remove(struct platform_device *pdev)
{
	struct hermes_kms_device *hdev = platform_get_drvdata(pdev);

	drm_dev_unregister(&hdev->drm);
	drm_atomic_helper_shutdown(&hdev->drm);
}

static void hermes_kms_platform_release(struct device *dev)
{
}

static struct platform_driver hermes_kms_platform_driver = {
	.probe = hermes_kms_probe,
	.remove = hermes_kms_remove,
	.driver = {
		.name = HERMES_KMS_DRIVER_NAME,
	},
};

static struct platform_device hermes_kms_platform_device = {
	.name = HERMES_KMS_DRIVER_NAME,
	.id = PLATFORM_DEVID_NONE,
	.dev = {
		.release = hermes_kms_platform_release,
	},
};

static int __init hermes_kms_init(void)
{
	int ret;

	ret = platform_driver_register(&hermes_kms_platform_driver);
	if (ret)
		return ret;

	ret = platform_device_register(&hermes_kms_platform_device);
	if (ret) {
		platform_driver_unregister(&hermes_kms_platform_driver);
		return ret;
	}

	pr_info("%s: module loaded\n", HERMES_KMS_DRIVER_NAME);
	return 0;
}

static void __exit hermes_kms_exit(void)
{
	platform_device_unregister(&hermes_kms_platform_device);
	platform_driver_unregister(&hermes_kms_platform_driver);
	pr_info("%s: module unloaded\n", HERMES_KMS_DRIVER_NAME);
}

module_init(hermes_kms_init);
module_exit(hermes_kms_exit);

MODULE_AUTHOR("Hermes contributors");
MODULE_DESCRIPTION(HERMES_KMS_DRIVER_DESC);
MODULE_LICENSE("GPL");
