// SPDX-License-Identifier: GPL-2.0
/*
 * Hermes-KMS - experimental DRM/KMS virtual display driver for Hermes.
 *
 * This module is intentionally not an EVDI wrapper.  The first milestone is a
 * safe DRM device/connector skeleton; DMA-BUF export and low-latency capture
 * will be built on top of this once the KMS object lifecycle is stable.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
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

struct hermes_kms_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
};

static inline struct hermes_kms_device *to_hermes_kms(struct drm_device *drm)
{
	return container_of(drm, struct hermes_kms_device, drm);
}

static const u32 hermes_kms_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static enum drm_connector_status
hermes_kms_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static int hermes_kms_connector_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_add_modes_noedid(connector, 3840, 2160);
	drm_set_preferred_mode(connector, 1920, 1080);

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
	if (mode->hdisplay < 640 || mode->vdisplay < 480)
		return MODE_BAD;

	if (mode->hdisplay > 3840 || mode->vdisplay > 2160)
		return MODE_VIRTUAL_X;

	if (drm_mode_vrefresh(mode) > 240)
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

	drm_info(drm, "enabled virtual display %ux%u@%d\n",
		 crtc_state->mode.hdisplay,
		 crtc_state->mode.vdisplay,
		 drm_mode_vrefresh(&crtc_state->mode));
}

static void hermes_kms_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	drm_info(pipe->crtc.dev, "disabled virtual display\n");
}

static void hermes_kms_pipe_update(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;

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
	DRM_GEM_DMA_DRIVER_OPS,
};

static int hermes_kms_modeset_init(struct hermes_kms_device *hdev)
{
	struct drm_device *drm = &hdev->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 640;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_width = 3840;
	drm->mode_config.max_height = 2160;
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
