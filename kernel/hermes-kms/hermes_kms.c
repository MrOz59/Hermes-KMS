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
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/fdtable.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/sync_file.h>
#include <linux/wait.h>

#include <drm/hermes_kms_drm.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_prime.h>
#include <drm/drm_vblank.h>

#define HERMES_KMS_DRIVER_NAME "hermes-kms"
#define HERMES_KMS_DRIVER_DESC "Hermes virtual KMS display"
#define HERMES_KMS_DRIVER_DATE "20260625"
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 1
#define HERMES_KMS_DRIVER_PATCH 2
#define HERMES_KMS_OUTPUT_NAME "HERMES-1"

#define HERMES_KMS_MIN_WIDTH 640
#define HERMES_KMS_MIN_HEIGHT 480
#define HERMES_KMS_MAX_WIDTH 3840
#define HERMES_KMS_MAX_HEIGHT 2160
#define HERMES_KMS_DEFAULT_WIDTH 1920
#define HERMES_KMS_DEFAULT_HEIGHT 1080
#define HERMES_KMS_DEFAULT_REFRESH_HZ 60
#define HERMES_KMS_MAX_REFRESH_HZ 240

static bool initial_enabled;
static bool hotplug_events = true;
static bool non_desktop;
static unsigned int initial_width = HERMES_KMS_DEFAULT_WIDTH;
static unsigned int initial_height = HERMES_KMS_DEFAULT_HEIGHT;
static unsigned int initial_refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;

module_param(initial_enabled, bool, 0644);
MODULE_PARM_DESC(initial_enabled, "Initial virtual output state");
module_param(hotplug_events, bool, 0644);
MODULE_PARM_DESC(hotplug_events, "Emit DRM hotplug events when output state changes");
module_param(non_desktop, bool, 0644);
MODULE_PARM_DESC(non_desktop, "Mark connector as non-desktop. Default false so compositors can manage Hermes as a normal virtual monitor when connected");
module_param(initial_width, uint, 0644);
MODULE_PARM_DESC(initial_width, "Initial virtual output width");
module_param(initial_height, uint, 0644);
MODULE_PARM_DESC(initial_height, "Initial virtual output height");
module_param(initial_refresh_hz, uint, 0644);
MODULE_PARM_DESC(initial_refresh_hz, "Initial virtual output refresh rate");

struct hermes_kms_device {
	struct drm_device drm;
	/*
	 * Explicit KMS objects (CRTC + encoder + primary plane), rather than
	 * drm_simple_display_pipe, so we can drive a software vblank timer the
	 * way vkms does. The simple pipe helper does not support timer-based
	 * vblank, which a virtual display needs to pace the compositor at the
	 * mode's refresh instead of its commit/ack loop (~40fps).
	 */
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_connector connector;
	struct mutex state_lock;
	wait_queue_head_t frame_wait;
	struct drm_framebuffer *framebuffer;
	struct drm_file *owner_file;
	/*
	 * Software vblank timer. A virtual display has no hardware vblank, so the
	 * compositor (KWin/GNOME) gets no periodic "present now" tick and ends up
	 * composing at the speed of its commit/ack loop (~40fps) instead of the
	 * mode's refresh rate. This hrtimer fires at the requested refresh so the
	 * compositor composes at the full rate (60/120/144Hz). Modelled on vkms.
	 */
	struct hrtimer vblank_timer;
	ktime_t vblank_period;  /* nanoseconds between vblanks for the active mode */
	/*
	 * Per-plane dma-buf export cache. drm_gem_prime_export() always
	 * allocates a fresh dma_buf, so re-exporting the same scanout BO every
	 * ACQUIRE_FRAME (60+ times per second) would force the consumer to
	 * re-import and re-map on every frame, defeating zero-copy. Cache the
	 * exported dma_buf keyed by the GEM object pointer and hand out a fresh
	 * fd referencing the cached dma_buf while the BO is unchanged. Protected
	 * by export_lock.
	 */
	struct mutex export_lock;
	struct drm_gem_object *export_obj[4];
	struct dma_buf *export_dmabuf[4];
	u64 session_id;
	u64 next_session_id;
	pid_t owner_pid;
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
	/*
	 * Damage region accumulated for the in-progress frame, set by the plane
	 * atomic_update from FB_DAMAGE_CLIPS and latched into the frame at flush.
	 * damage_valid is cleared when the compositor provides no damage (treat
	 * the whole frame as dirty). Protected by state_lock.
	 */
	bool framebuffer_damage_valid;
	u32 framebuffer_damage_x1;
	u32 framebuffer_damage_y1;
	u32 framebuffer_damage_x2;
	u32 framebuffer_damage_y2;
	u64 frame_update_count;
	u64 vblank_count;	   /* vblanks the software timer has fired */
	u64 vblank_overrun_count;  /* timer ticks that fell behind (dropped flips) */
	u64 acquire_count;
	u64 acquire_no_frame_count;
	u64 dmabuf_export_count;
	u64 dmabuf_export_fail_count;
	u64 sync_file_export_count;
	u64 sync_file_export_fail_count;
	u64 wait_count;
	u64 wait_ready_count;
	u64 wait_timeout_count;
	u64 wait_interrupted_count;
	u64 output_enable_count;
	u64 output_disable_count;
	u64 hotplug_event_count;
	u64 owner_close_disconnect_count;
	u64 last_acquire_ns;
	u64 last_wait_start_ns;
	u64 last_wait_end_ns;
	u64 last_wait_duration_ns;
	u64 last_dmabuf_export_ns;
	u64 last_sync_file_export_ns;
	u64 last_logged_framebuffer_id;
	u64 acquire_no_frame_log_count;
};

static inline struct hermes_kms_device *to_hermes_kms(struct drm_device *drm)
{
	return container_of(drm, struct hermes_kms_device, drm);
}

/* Drop every cached dma-buf export. Caller must hold export_lock. */
static void hermes_kms_drop_export_cache_locked(struct hermes_kms_device *hdev)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hdev->export_dmabuf); i++) {
		if (hdev->export_dmabuf[i]) {
			dma_buf_put(hdev->export_dmabuf[i]);
			hdev->export_dmabuf[i] = NULL;
		}
		hdev->export_obj[i] = NULL;
	}
}

static const u32 hermes_kms_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

/* The cursor plane only needs the standard alpha cursor format. */
static const u32 hermes_kms_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/*
 * Synthetic EDID 1.3 base block identifying the Hermes virtual monitor.
 * Compositors (e.g. KWin) warn and may refuse to configure a connector with
 * no EDID ("Could not find edid for connector"). This block provides identity
 * (manufacturer "HRM", name "Hermes KMS") and sane range limits so the output
 * is treated as a normal monitor. The actual mode list is still generated
 * dynamically in get_modes() via CVT so arbitrary client geometries work; the
 * EDID's detailed timing is only a fallback/preferred hint. Checksum verified.
 */
static const u8 hermes_kms_edid[128] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x22, 0x4d, 0x01, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x22, 0x01, 0x03, 0x80, 0x00, 0x00, 0x78,
	0x02, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
	0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x40, 0x84, 0x63, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x48, 0x65, 0x72, 0x6d, 0x65, 0x73, 0x20,
	0x4b, 0x4d, 0x53, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17,
	0x4b, 0x0f, 0x96, 0x1e, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86,
};

static bool hermes_kms_hotplug_event(struct drm_device *drm)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);

	if (!hotplug_events)
		return false;

	mutex_lock(&hdev->state_lock);
	hdev->hotplug_event_count++;
	mutex_unlock(&hdev->state_lock);

	drm_kms_helper_hotplug_event(drm);
	return true;
}

/*
 * Re-probe the connector so get_modes() runs again with the current
 * requested geometry. A client that requests an arbitrary mode via
 * SET_OUTPUT (e.g. 1280x720@30) needs the connector to actually advertise
 * that mode before a modeset can succeed. The userspace GETCONNECTOR path
 * re-probes on its own, but a consumer that already cached the mode list -
 * or a setup running with hotplug_events disabled - would otherwise never
 * see the new mode. Force the probe directly; it is independent of the
 * hotplug_events module parameter, which only gates connection-state events.
 */
static void hermes_kms_reprobe_modes(struct drm_device *drm)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);

	mutex_lock(&drm->mode_config.mutex);
	drm_helper_probe_single_connector_modes(&hdev->connector,
						HERMES_KMS_MAX_WIDTH,
						HERMES_KMS_MAX_HEIGHT);
	mutex_unlock(&drm->mode_config.mutex);
}

static u64 hermes_kms_next_session_id_locked(struct hermes_kms_device *hdev)
{
	u64 session_id = hdev->next_session_id++;

	if (!hdev->next_session_id)
		hdev->next_session_id = 1;

	return session_id;
}

static void hermes_kms_clear_owner_locked(struct hermes_kms_device *hdev)
{
	hdev->owner_file = NULL;
	hdev->owner_pid = 0;
	hdev->session_id = 0;
}

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

static void hermes_kms_set_frame_metadata_locked(struct hermes_kms_device *hdev,
						 struct drm_framebuffer *fb)
{
	unsigned int i;
	unsigned int plane_count = 0;

	if (!fb) {
		hermes_kms_clear_frame_locked(hdev);
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
}

static void hermes_kms_track_frame(struct hermes_kms_device *hdev,
				   struct drm_framebuffer *fb)
{
	struct drm_device *drm = &hdev->drm;
	struct drm_framebuffer *old_fb;
	u32 old_fb_id;
	u64 sequence;
	bool log_frame_connected = false;
	bool log_frame_disconnected = false;

	if (fb)
		drm_framebuffer_get(fb);

	mutex_lock(&hdev->state_lock);
	old_fb = hdev->framebuffer;
	old_fb_id = old_fb ? old_fb->base.id : 0;
	hdev->framebuffer = fb;
	hdev->frame_sequence++;
	hdev->frame_update_count++;
	hdev->last_update_ns = ktime_get_ns();
	sequence = hdev->frame_sequence;
	hermes_kms_set_frame_metadata_locked(hdev, fb);
	if (fb && !old_fb_id) {
		hdev->last_logged_framebuffer_id = fb->base.id;
		log_frame_connected = true;
	} else if (!fb && old_fb_id) {
		hdev->last_logged_framebuffer_id = 0;
		log_frame_disconnected = true;
	} else if (fb && hdev->last_logged_framebuffer_id != fb->base.id) {
		hdev->last_logged_framebuffer_id = fb->base.id;
		drm_dbg_kms(drm,
			    "scanout framebuffer changed: id=%u size=%ux%u format=0x%08x modifier=0x%016llx planes=%u sequence=%llu\n",
			    hdev->framebuffer_id,
			    hdev->framebuffer_width,
			    hdev->framebuffer_height,
			    hdev->framebuffer_format,
			    (unsigned long long)hdev->framebuffer_modifier,
			    hdev->framebuffer_plane_count,
			    (unsigned long long)hdev->frame_sequence);
	}
	mutex_unlock(&hdev->state_lock);

	/*
	 * When the scanout framebuffer goes away (output disabled), drop the
	 * export cache so we do not pin the consumer's imported buffers. While
	 * a framebuffer is present the cache self-corrects: the export path
	 * re-exports whenever the compositor flips to a different BO. Taken
	 * outside state_lock to keep export_lock strictly below it.
	 */
	if (!fb) {
		mutex_lock(&hdev->export_lock);
		hermes_kms_drop_export_cache_locked(hdev);
		mutex_unlock(&hdev->export_lock);
	}

	if (old_fb)
		drm_framebuffer_put(old_fb);

	if (log_frame_connected)
		drm_info(drm,
			 "first active scanout framebuffer: id=%u size=%ux%u format=0x%08x modifier=0x%016llx planes=%u sequence=%llu\n",
			 fb->base.id, fb->width, fb->height, fb->format->format,
			 (unsigned long long)fb->modifier, fb->format->num_planes,
			 (unsigned long long)sequence);
	else if (log_frame_disconnected)
		drm_info(drm, "cleared active scanout framebuffer\n");

	wake_up_interruptible(&hdev->frame_wait);
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
	int count = 0;
	struct hermes_kms_device *hdev = to_hermes_kms(connector->dev);
	const struct drm_edid *drm_edid;
	struct drm_display_mode *mode;
	u32 width;
	u32 height;
	u32 refresh_hz;

	mutex_lock(&hdev->state_lock);
	width = hdev->requested_width;
	height = hdev->requested_height;
	refresh_hz = hdev->requested_refresh_hz;
	mutex_unlock(&hdev->state_lock);

	/*
	 * Attach the synthetic EDID for identity. This makes compositors treat
	 * the connector as a normal monitor (name, manufacturer, range limits)
	 * instead of warning about a missing EDID. drm_edid_connector_update()
	 * also records the EDID so userspace can read it back. The EDID's own
	 * detailed-timing modes are added too, but the CVT mode below is marked
	 * preferred so the client's exact geometry still wins.
	 */
	drm_edid = drm_edid_alloc(hermes_kms_edid, sizeof(hermes_kms_edid));
	if (drm_edid) {
		drm_edid_connector_update(connector, drm_edid);
		count += drm_edid_connector_add_modes(connector);
		drm_edid_free(drm_edid);
	}

	/*
	 * Synthesize the exact mode the userspace owner asked for. A remote
	 * streaming client can request arbitrary geometry/refresh (e.g.
	 * 1280x720@30), which neither the EDID nor the generic ladder contains.
	 * Without a matching mode the compositor/modetest atomic commit fails
	 * with "failed to find mode". Add it via CVT and mark it preferred so it
	 * is selected by default.
	 */
	if (width && height && refresh_hz) {
		mode = drm_cvt_mode(connector->dev, width, height, refresh_hz,
				    false, false, false);
		if (mode) {
			mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
			drm_mode_probed_add(connector, mode);
			count++;
			drm_info(connector->dev,
				 "get_modes added preferred CVT mode %ux%u clock=%d vrefresh=%d name=%s\n",
				 width, height, mode->clock,
				 drm_mode_vrefresh(mode), mode->name);
		} else {
			drm_warn(connector->dev,
				 "get_modes: drm_cvt_mode(%ux%u@%u) returned NULL\n",
				 width, height, refresh_hz);
		}
	}

	/*
	 * Also expose the standard mode ladder so a client can pick a common
	 * resolution without a SET_OUTPUT round-trip, and so the connector is
	 * never left with zero modes if CVT synthesis fails.
	 */
	count += drm_add_modes_noedid(connector, HERMES_KMS_MAX_WIDTH,
				      HERMES_KMS_MAX_HEIGHT);

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

static inline struct hermes_kms_device *crtc_to_hermes_kms(struct drm_crtc *crtc)
{
	return container_of(crtc, struct hermes_kms_device, crtc);
}

static inline struct hermes_kms_device *
plane_to_hermes_kms(struct drm_plane *plane)
{
	return container_of(plane, struct hermes_kms_device, primary);
}

/*
 * Software vblank timer callback. Mirrors vkms: roll the timer forward, then
 * signal the vblank to DRM (which delivers any armed page-flip event and lets
 * the compositor schedule the next frame at the mode's refresh rate).
 */
static enum hrtimer_restart hermes_kms_vblank_timer(struct hrtimer *timer)
{
	struct hermes_kms_device *hdev =
		container_of(timer, struct hermes_kms_device, vblank_timer);
	struct drm_crtc *crtc = &hdev->crtc;
	u64 ret_overrun;
	bool ret;

	ret_overrun = hrtimer_forward_now(&hdev->vblank_timer,
					  hdev->vblank_period);
	if (ret_overrun != 1) {
		/*
		 * More than one period elapsed since the last tick: the timer
		 * fell behind and at least one vblank (page-flip slot) was
		 * missed. Track it so the pacing self-test can assert that a
		 * steady stream produces zero overruns. ret_overrun counts the
		 * periods skipped over (>=2 here, or 0 if called late-but-once).
		 */
		WRITE_ONCE(hdev->vblank_overrun_count,
			   hdev->vblank_overrun_count +
				   (ret_overrun > 1 ? ret_overrun - 1 : 1));
		drm_dbg_kms(&hdev->drm, "vblank timer overrun (skipped=%llu)\n",
			    (unsigned long long)ret_overrun);
	}
	WRITE_ONCE(hdev->vblank_count, hdev->vblank_count + 1);

	ret = drm_crtc_handle_vblank(crtc);
	if (!ret)
		drm_err(&hdev->drm, "hermes-kms failure on handling vblank\n");

	return HRTIMER_RESTART;
}

static int hermes_kms_enable_vblank(struct drm_crtc *crtc)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	unsigned int refresh = drm_mode_vrefresh(&crtc->mode);

	if (!refresh)
		refresh = HERMES_KMS_DEFAULT_REFRESH_HZ;

	/*
	 * Prefer the DRM-calculated per-frame duration (set up by
	 * drm_calc_timestamping_constants() in the modeset path); fall back to a
	 * direct computation if it is not available. This honours the active
	 * mode's refresh, so 60/120/144Hz all work.
	 */
	if (vblank && vblank->framedur_ns)
		hdev->vblank_period = ns_to_ktime(vblank->framedur_ns);
	else
		hdev->vblank_period = ns_to_ktime(NSEC_PER_SEC / refresh);

	hrtimer_start(&hdev->vblank_timer, hdev->vblank_period,
		      HRTIMER_MODE_REL);

	return 0;
}

static void hermes_kms_disable_vblank(struct drm_crtc *crtc)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);

	hrtimer_cancel(&hdev->vblank_timer);
}

/*
 * Provide a vblank timestamp from the timer. Without this hook DRM cannot
 * timestamp timer-driven vblanks and drm_crtc_handle_vblank() misbehaves.
 * Mirrors vkms.
 */
static bool hermes_kms_get_vblank_timestamp(struct drm_crtc *crtc,
					    int *max_error,
					    ktime_t *vblank_time,
					    bool in_vblank_irq)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);

	if (!READ_ONCE(vblank->enabled)) {
		*vblank_time = ktime_get();
		return true;
	}

	*vblank_time = READ_ONCE(hdev->vblank_timer.node.expires);

	if (WARN_ON(*vblank_time == vblank->time))
		return true;

	/* The timer was rolled forward before processing, so correct by one. */
	*vblank_time -= hdev->vblank_period;

	return true;
}

static const struct drm_crtc_funcs hermes_kms_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = hermes_kms_enable_vblank,
	.disable_vblank = hermes_kms_disable_vblank,
	.get_vblank_timestamp = hermes_kms_get_vblank_timestamp,
};

static enum drm_mode_status
hermes_kms_plane_mode_valid(const struct drm_display_mode *mode);

static int hermes_kms_crtc_atomic_check(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (!crtc_state->enable)
		return 0;

	/*
	 * Validate the requested mode against this driver's limits up front so a
	 * malformed commit is rejected at check time (before any hardware-ish
	 * state is touched), rather than silently scanning out garbage. The
	 * single plane is the primary, so an enabled+active CRTC must drive a
	 * framebuffer; refuse an active CRTC with nothing to scan out.
	 */
	if (hermes_kms_plane_mode_valid(&crtc_state->mode) != MODE_OK) {
		drm_dbg_kms(crtc->dev,
			    "atomic_check: rejecting mode %ux%u@%d (out of range)\n",
			    crtc_state->mode.hdisplay,
			    crtc_state->mode.vdisplay,
			    drm_mode_vrefresh(&crtc_state->mode));
		return -EINVAL;
	}

	/*
	 * Only a full modeset may change the active mode; let the helper enforce
	 * the standard connector/encoder routing and event invariants too.
	 */
	ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
	if (ret) {
		drm_dbg_kms(crtc->dev,
			    "atomic_check: active CRTC has no primary plane (%d)\n",
			    ret);
		return ret;
	}

	return 0;
}

static void hermes_kms_crtc_atomic_enable(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);

	mutex_lock(&hdev->state_lock);
	hdev->last_enable_ns = ktime_get_ns();
	mutex_unlock(&hdev->state_lock);

	/*
	 * Compute the vblank timestamping constants (framedur_ns/linedur_ns)
	 * for the active mode before enabling vblank, so the vblank timer period
	 * and get_vblank_timestamp() are accurate. Required for timer-driven
	 * vblank; without it framedur_ns stays zero.
	 */
	drm_calc_timestamping_constants(crtc, &crtc->state->mode);

	drm_crtc_vblank_on(crtc);

	drm_info(&hdev->drm, "enabled virtual display %ux%u@%d\n",
		 crtc->state->mode.hdisplay,
		 crtc->state->mode.vdisplay,
		 drm_mode_vrefresh(&crtc->state->mode));
}

static void hermes_kms_crtc_atomic_disable(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);

	drm_crtc_vblank_off(crtc);

	mutex_lock(&hdev->state_lock);
	hdev->last_disable_ns = ktime_get_ns();
	mutex_unlock(&hdev->state_lock);
	hermes_kms_track_frame(hdev, NULL);

	drm_info(&hdev->drm, "disabled virtual display\n");
}

static void hermes_kms_crtc_atomic_flush(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct hermes_kms_device *hdev = crtc_to_hermes_kms(crtc);
	struct drm_plane *plane = &hdev->primary;
	struct drm_framebuffer *fb = plane->state ? plane->state->fb : NULL;

	hermes_kms_track_frame(hdev, fb);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		/*
		 * Pace the flip completion to the next vblank tick when vblank is
		 * on (matching vkms). The reference from drm_crtc_vblank_get() is
		 * released by DRM when the armed event fires; do not put it here.
		 */
		if (drm_crtc_vblank_get(crtc) != 0)
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);

		crtc->state->event = NULL;
	}
}

static const struct drm_crtc_helper_funcs hermes_kms_crtc_helper_funcs = {
	.atomic_check = hermes_kms_crtc_atomic_check,
	.atomic_enable = hermes_kms_crtc_atomic_enable,
	.atomic_disable = hermes_kms_crtc_atomic_disable,
	.atomic_flush = hermes_kms_crtc_atomic_flush,
};

static enum drm_mode_status
hermes_kms_plane_mode_valid(const struct drm_display_mode *mode)
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

static int hermes_kms_plane_atomic_check(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, true);
	if (ret)
		return ret;

	if (new_state->fb) {
		enum drm_mode_status status =
			hermes_kms_plane_mode_valid(&crtc_state->mode);

		if (status != MODE_OK)
			return -EINVAL;
	}

	return 0;
}

static void hermes_kms_plane_atomic_update(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct hermes_kms_device *hdev = plane_to_hermes_kms(plane);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_rect damage;
	bool have_damage = false;

	/*
	 * Capture the compositor's FB_DAMAGE_CLIPS for this commit and latch it
	 * so ACQUIRE_FRAME can hand the consumer a single merged dirty rect
	 * (the consumer encodes only the changed region). If no damage was
	 * supplied, fall back to "whole frame dirty" so correctness never
	 * depends on a compositor providing damage. Actual scanout bookkeeping
	 * still happens in the CRTC atomic_flush.
	 */
	if (new_state && new_state->fb && new_state->crtc)
		have_damage = drm_atomic_helper_damage_merged(old_state,
							      new_state,
							      &damage);

	mutex_lock(&hdev->state_lock);
	if (have_damage) {
		hdev->framebuffer_damage_valid = true;
		hdev->framebuffer_damage_x1 = max(damage.x1, 0);
		hdev->framebuffer_damage_y1 = max(damage.y1, 0);
		hdev->framebuffer_damage_x2 = max(damage.x2, 0);
		hdev->framebuffer_damage_y2 = max(damage.y2, 0);
	} else {
		hdev->framebuffer_damage_valid = false;
	}
	mutex_unlock(&hdev->state_lock);
}

static const struct drm_plane_helper_funcs hermes_kms_plane_helper_funcs = {
	.atomic_check = hermes_kms_plane_atomic_check,
	.atomic_update = hermes_kms_plane_atomic_update,
};

static const struct drm_plane_funcs hermes_kms_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/*
 * Cursor plane. Exposing a cursor plane lets the compositor (KWin/GNOME) move
 * the pointer without recompositing the whole output every frame, which keeps
 * the captured primary framebuffer stable on cursor-only motion. The capture
 * consumer (Apollo) detects the cursor plane separately and renders it
 * client-side (Moonlight overlays the cursor), so the cursor does not need to
 * be blended into the streamed primary plane here.
 */
static int hermes_kms_cursor_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	/* Cursor may sit partly off-screen and is never scaled or clipped. */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   true, true);
}

static void hermes_kms_cursor_atomic_update(struct drm_plane *plane,
					    struct drm_atomic_state *state)
{
	/*
	 * Nothing to scan out: the cursor is consumed client-side. The update
	 * exists so the compositor's cursor commits succeed and stay off the
	 * primary plane's damage path.
	 */
}

static const struct drm_plane_helper_funcs hermes_kms_cursor_helper_funcs = {
	.atomic_check = hermes_kms_cursor_atomic_check,
	.atomic_update = hermes_kms_cursor_atomic_update,
};

static const struct drm_plane_funcs hermes_kms_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_encoder_funcs hermes_kms_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
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
		      HERMES_KMS_CAP_DMABUF_EXPORT |
		      HERMES_KMS_CAP_OUTPUT_IDENTITY |
		      HERMES_KMS_CAP_SESSION_OWNER |
		      HERMES_KMS_CAP_FRAME_WAIT |
		      HERMES_KMS_CAP_METRICS |
		      HERMES_KMS_CAP_ZERO_COPY_TARGET |
		      HERMES_KMS_CAP_SYNC_FILE;
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
	if (hotplug_events)
		status->flags |= HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED;
	if (hdev->owner_file)
		status->flags |= HERMES_KMS_STATUS_SESSION_OWNED;

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
	status->session_id = hdev->session_id;
	status->owner_pid = hdev->owner_pid ? hdev->owner_pid : -1;
	if (hdev->framebuffer_id)
		status->flags |= HERMES_KMS_STATUS_FRAME_VALID;
	if (hdev->framebuffer)
		status->flags |= HERMES_KMS_STATUS_DMABUF_EXPORT_READY;
	mutex_unlock(&hdev->state_lock);

	status->connector_id = hdev->connector.base.id;
	status->crtc_id = hdev->crtc.base.id;
	status->plane_id = hdev->primary.base.id;
	status->encoder_id = hdev->encoder.base.id;

	/*
	 * crtc->state is swapped under the CRTC modeset lock during an atomic
	 * commit; take it so we never dereference a state being freed. This is
	 * a diagnostic path, so blocking briefly on a concurrent commit is fine.
	 */
	drm_modeset_lock(&hdev->crtc.mutex, NULL);
	crtc_state = hdev->crtc.state;
	if (crtc_state && crtc_state->enable) {
		status->flags |= HERMES_KMS_STATUS_SCANOUT_ACTIVE;
		status->active_width = crtc_state->mode.hdisplay;
		status->active_height = crtc_state->mode.vdisplay;
		status->active_refresh_hz = drm_mode_vrefresh(&crtc_state->mode);
	}
	drm_modeset_unlock(&hdev->crtc.mutex);

	return 0;
}

static int hermes_kms_ioctl_get_metrics(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_metrics *metrics = data;

	memset(metrics, 0, sizeof(*metrics));

	mutex_lock(&hdev->state_lock);
	metrics->frame_sequence = hdev->frame_sequence;
	metrics->frame_update_count = hdev->frame_update_count;
	metrics->acquire_count = hdev->acquire_count;
	metrics->acquire_no_frame_count = hdev->acquire_no_frame_count;
	metrics->dmabuf_export_count = hdev->dmabuf_export_count;
	metrics->dmabuf_export_fail_count = hdev->dmabuf_export_fail_count;
	metrics->sync_file_export_count = hdev->sync_file_export_count;
	metrics->sync_file_export_fail_count = hdev->sync_file_export_fail_count;
	metrics->wait_count = hdev->wait_count;
	metrics->wait_ready_count = hdev->wait_ready_count;
	metrics->wait_timeout_count = hdev->wait_timeout_count;
	metrics->wait_interrupted_count = hdev->wait_interrupted_count;
	metrics->output_enable_count = hdev->output_enable_count;
	metrics->output_disable_count = hdev->output_disable_count;
	metrics->hotplug_event_count = hdev->hotplug_event_count;
	metrics->owner_close_disconnect_count = hdev->owner_close_disconnect_count;
	metrics->last_update_ns = hdev->last_update_ns;
	metrics->last_acquire_ns = hdev->last_acquire_ns;
	metrics->last_wait_start_ns = hdev->last_wait_start_ns;
	metrics->last_wait_end_ns = hdev->last_wait_end_ns;
	metrics->last_wait_duration_ns = hdev->last_wait_duration_ns;
	metrics->last_dmabuf_export_ns = hdev->last_dmabuf_export_ns;
	metrics->last_sync_file_export_ns = hdev->last_sync_file_export_ns;
	mutex_unlock(&hdev->state_lock);

	return 0;
}

static int hermes_kms_ioctl_get_identity(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_identity *identity = data;
	const char *connector_name = hdev->connector.name;

	memset(identity, 0, sizeof(*identity));
	strscpy(identity->driver_name, HERMES_KMS_DRIVER_NAME,
		sizeof(identity->driver_name));
	strscpy(identity->output_name, HERMES_KMS_OUTPUT_NAME,
		sizeof(identity->output_name));
	if (connector_name)
		strscpy(identity->connector_name, connector_name,
			sizeof(identity->connector_name));

	identity->connector_id = hdev->connector.base.id;
	identity->crtc_id = hdev->crtc.base.id;
	identity->plane_id = hdev->primary.base.id;
	identity->encoder_id = hdev->encoder.base.id;

	return 0;
}

static void hermes_kms_fill_wait_frame_locked(struct hermes_kms_device *hdev,
					      struct drm_hermes_kms_wait_frame *wait)
{
	wait->sequence = hdev->frame_sequence;
	wait->timestamp_ns = hdev->last_update_ns;
	wait->status_flags = 0;

	if (hdev->output_enabled)
		wait->status_flags |= HERMES_KMS_STATUS_OUTPUT_ENABLED |
				      HERMES_KMS_STATUS_CONNECTED;
	if (hotplug_events)
		wait->status_flags |= HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED;
	if (hdev->owner_file)
		wait->status_flags |= HERMES_KMS_STATUS_SESSION_OWNED;
	if (hdev->framebuffer_id)
		wait->status_flags |= HERMES_KMS_STATUS_FRAME_VALID;
	if (hdev->framebuffer) {
		wait->flags |= HERMES_KMS_WAIT_FRAME_READY;
		wait->status_flags |= HERMES_KMS_STATUS_DMABUF_EXPORT_READY;
	}
}

static int hermes_kms_ioctl_wait_frame(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_wait_frame *wait = data;
	u64 after_sequence = wait->after_sequence;
	u32 timeout_ms = wait->timeout_ms;
	u64 start_ns = ktime_get_ns();
	u64 end_ns;
	long timeout;

	mutex_lock(&hdev->state_lock);
	hdev->wait_count++;
	hdev->last_wait_start_ns = start_ns;
	mutex_unlock(&hdev->state_lock);

	if (READ_ONCE(hdev->frame_sequence) <= after_sequence) {
		if (!timeout_ms) {
			mutex_lock(&hdev->state_lock);
			hdev->wait_timeout_count++;
			hdev->last_wait_end_ns = ktime_get_ns();
			hdev->last_wait_duration_ns =
				hdev->last_wait_end_ns - start_ns;
			mutex_unlock(&hdev->state_lock);
			return -EAGAIN;
		}

		timeout = wait_event_interruptible_timeout(
			hdev->frame_wait,
			READ_ONCE(hdev->frame_sequence) > after_sequence,
			msecs_to_jiffies(timeout_ms));
		if (timeout < 0) {
			mutex_lock(&hdev->state_lock);
			hdev->wait_interrupted_count++;
			hdev->last_wait_end_ns = ktime_get_ns();
			hdev->last_wait_duration_ns =
				hdev->last_wait_end_ns - start_ns;
			mutex_unlock(&hdev->state_lock);
			return timeout;
		}
		if (!timeout) {
			mutex_lock(&hdev->state_lock);
			hdev->wait_timeout_count++;
			hdev->last_wait_end_ns = ktime_get_ns();
			hdev->last_wait_duration_ns =
				hdev->last_wait_end_ns - start_ns;
			mutex_unlock(&hdev->state_lock);
			return -ETIMEDOUT;
		}
	}

	memset(wait, 0, sizeof(*wait));

	end_ns = ktime_get_ns();
	mutex_lock(&hdev->state_lock);
	hdev->wait_ready_count++;
	hdev->last_wait_end_ns = end_ns;
	hdev->last_wait_duration_ns = end_ns - start_ns;
	hermes_kms_fill_wait_frame_locked(hdev, wait);
	mutex_unlock(&hdev->state_lock);

	return 0;
}

static void hermes_kms_init_invalid_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frame->dma_buf_fd); i++)
		frame->dma_buf_fd[i] = -1;

	frame->sync_file_fd = -1;
}

static void hermes_kms_close_frame_fds(struct drm_hermes_kms_acquire_frame *frame)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frame->dma_buf_fd); i++) {
		if (frame->dma_buf_fd[i] >= 0) {
			close_fd(frame->dma_buf_fd[i]);
			frame->dma_buf_fd[i] = -1;
		}
	}

	if (frame->sync_file_fd >= 0) {
		close_fd(frame->sync_file_fd);
		frame->sync_file_fd = -1;
	}
}

/*
 * Return a dma_buf for plane @index of @fb, reusing the cached export when the
 * underlying GEM object has not changed. The returned dma_buf carries an extra
 * reference owned by the caller (released via dma_buf_put, typically through
 * the installed fd). Caller must hold export_lock.
 */
static struct dma_buf *
hermes_kms_get_plane_dmabuf_locked(struct hermes_kms_device *hdev,
				   struct drm_framebuffer *fb,
				   unsigned int index)
{
	struct drm_gem_object *obj;
	struct dma_buf *dmabuf;

	obj = drm_gem_fb_get_obj(fb, index);
	if (!obj)
		return ERR_PTR(-EINVAL);

	if (hdev->export_obj[index] == obj && hdev->export_dmabuf[index]) {
		get_dma_buf(hdev->export_dmabuf[index]);
		return hdev->export_dmabuf[index];
	}

	dmabuf = drm_gem_prime_export(obj, O_RDWR);
	if (IS_ERR(dmabuf))
		return dmabuf;

	/* Replace the cache entry; the cache holds one reference. */
	if (hdev->export_dmabuf[index])
		dma_buf_put(hdev->export_dmabuf[index]);
	get_dma_buf(dmabuf);
	hdev->export_dmabuf[index] = dmabuf;
	hdev->export_obj[index] = obj;

	return dmabuf;
}

static int hermes_kms_export_frame_dmabufs(struct hermes_kms_device *hdev,
					   struct drm_framebuffer *fb,
					   struct drm_hermes_kms_acquire_frame *frame)
{
	unsigned int i;
	int ret = 0;

	mutex_lock(&hdev->export_lock);
	for (i = 0; i < frame->plane_count; i++) {
		struct dma_buf *dmabuf;
		int fd;

		dmabuf = hermes_kms_get_plane_dmabuf_locked(hdev, fb, i);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			break;
		}

		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			/* dma_buf_fd does not consume the ref on failure. */
			dma_buf_put(dmabuf);
			ret = fd;
			break;
		}

		frame->dma_buf_fd[i] = fd;
	}
	mutex_unlock(&hdev->export_lock);

	if (ret)
		return ret;

	frame->flags |= HERMES_KMS_FRAME_DMABUF_VALID;
	frame->flags &= ~HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED;
	return 0;
}

/*
 * Build a fence representing when the scanout buffer is safe for the consumer
 * to read. Prefer the buffer's implicit write fences (dma_resv): the compositor
 * may have flipped the framebuffer while its GPU still had pending render work,
 * and the consumer (e.g. VAAPI) must wait for that before sampling. Falls back
 * to an already-signalled stub when the buffer is idle, which is the common
 * case for this driver's synchronous update path.
 */
static struct dma_fence *
hermes_kms_frame_fence(struct drm_framebuffer *fb,
		       const struct drm_hermes_kms_acquire_frame *frame)
{
	struct drm_gem_object *obj;
	struct dma_fence *fence = NULL;

	obj = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	if (obj && obj->resv) {
		int ret;

		ret = dma_resv_get_singleton(obj->resv, DMA_RESV_USAGE_WRITE,
					     &fence);
		if (ret)
			fence = NULL;
	}

	if (!fence)
		fence = dma_fence_allocate_private_stub(
			ns_to_ktime(frame->timestamp_ns));

	return fence;
}

static int hermes_kms_export_sync_file(struct drm_framebuffer *fb,
				       struct drm_hermes_kms_acquire_frame *frame)
{
	struct dma_fence *fence;
	struct sync_file *sync_file;
	int fd;

	fence = hermes_kms_frame_fence(fb, frame);
	if (!fence)
		return -ENOMEM;

	sync_file = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync_file)
		return -ENOMEM;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sync_file->file);
		return fd;
	}

	fd_install(fd, sync_file->file);
	frame->sync_file_fd = fd;
	frame->flags |= HERMES_KMS_FRAME_SYNC_FILE_VALID;
	return 0;
}

static int hermes_kms_ioctl_acquire_frame(struct drm_device *drm, void *data,
					  struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct drm_hermes_kms_acquire_frame *frame = data;
	struct drm_framebuffer *fb;
	u64 requested_flags = frame->flags;
	int ret = 0;

	memset(frame, 0, sizeof(*frame));
	hermes_kms_init_invalid_frame_fds(frame);

	mutex_lock(&hdev->state_lock);
	hdev->acquire_count++;
	hdev->last_acquire_ns = ktime_get_ns();
	if (!hdev->framebuffer) {
		hdev->acquire_no_frame_count++;
		if (hdev->acquire_no_frame_log_count < 5) {
			hdev->acquire_no_frame_log_count++;
			drm_info(drm,
				 "ACQUIRE_FRAME has no framebuffer yet: output_enabled=%d owner_pid=%d session=%llu sequence=%llu\n",
				 hdev->output_enabled,
				 hdev->owner_pid ? hdev->owner_pid : -1,
				 (unsigned long long)hdev->session_id,
				 (unsigned long long)hdev->frame_sequence);
		} else {
			drm_dbg_kms_ratelimited(drm,
						"ACQUIRE_FRAME has no framebuffer yet: output_enabled=%d owner_pid=%d session=%llu sequence=%llu\n",
						hdev->output_enabled,
						hdev->owner_pid ? hdev->owner_pid : -1,
						(unsigned long long)hdev->session_id,
						(unsigned long long)hdev->frame_sequence);
		}
		mutex_unlock(&hdev->state_lock);
		return -ENODATA;
	}

	fb = hdev->framebuffer;
	drm_framebuffer_get(fb);
	frame->flags = HERMES_KMS_FRAME_METADATA_VALID;
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
	if (hdev->framebuffer_damage_valid) {
		frame->flags |= HERMES_KMS_FRAME_DAMAGE_VALID;
		frame->damage_x1 = hdev->framebuffer_damage_x1;
		frame->damage_y1 = hdev->framebuffer_damage_y1;
		frame->damage_x2 = hdev->framebuffer_damage_x2;
		frame->damage_y2 = hdev->framebuffer_damage_y2;
	}
	mutex_unlock(&hdev->state_lock);

	if (requested_flags & HERMES_KMS_FRAME_REQUEST_DMABUF) {
		ret = hermes_kms_export_frame_dmabufs(hdev, fb, frame);
		mutex_lock(&hdev->state_lock);
		if (ret) {
			hdev->dmabuf_export_fail_count++;
			mutex_unlock(&hdev->state_lock);
			hermes_kms_close_frame_fds(frame);
		} else {
			hdev->dmabuf_export_count++;
			hdev->last_dmabuf_export_ns = ktime_get_ns();
			mutex_unlock(&hdev->state_lock);
		}
		if (ret)
			goto out_put_fb;
	} else {
		frame->flags |= HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED;
	}

	if (requested_flags & HERMES_KMS_FRAME_REQUEST_SYNC_FILE) {
		ret = hermes_kms_export_sync_file(fb, frame);
		mutex_lock(&hdev->state_lock);
		if (ret) {
			hdev->sync_file_export_fail_count++;
			mutex_unlock(&hdev->state_lock);
			hermes_kms_close_frame_fds(frame);
		} else {
			hdev->sync_file_export_count++;
			hdev->last_sync_file_export_ns = ktime_get_ns();
			mutex_unlock(&hdev->state_lock);
		}
	}

out_put_fb:
	drm_framebuffer_put(fb);
	return ret;
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
	pid_t owner_pid;
	bool hotplug_sent;

	if (request->flags)
		return -EINVAL;

	request->result_flags = 0;
	request->session_id = 0;

	if (!request->enabled) {
		u64 session_id;

		mutex_lock(&hdev->state_lock);
		if (hdev->owner_file && hdev->owner_file != file) {
			mutex_unlock(&hdev->state_lock);
			return -EPERM;
		}

		session_id = hdev->session_id;
		owner_pid = hdev->owner_pid;
		hdev->output_enabled = false;
		hermes_kms_clear_owner_locked(hdev);
		hdev->last_disable_ns = ktime_get_ns();
		hdev->output_disable_count++;
		mutex_unlock(&hdev->state_lock);
		hermes_kms_track_frame(hdev, NULL);
		hotplug_sent = hermes_kms_hotplug_event(drm);
		request->session_id = session_id;
		if (hotplug_sent)
			request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT;
		drm_info(drm,
			 "disconnected virtual output session=%llu owner_pid=%d hotplug_sent=%d\n",
			 (unsigned long long)session_id,
			 owner_pid ? owner_pid : -1,
			 hotplug_sent);
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
	if (hdev->owner_file && hdev->owner_file != file) {
		mutex_unlock(&hdev->state_lock);
		return -EBUSY;
	}

	hdev->output_enabled = true;
	if (hdev->owner_file != file || !hdev->session_id) {
		hdev->owner_file = file;
		hdev->owner_pid = task_pid_nr(current);
		hdev->session_id = hermes_kms_next_session_id_locked(hdev);
	}
	hdev->requested_width = width;
	hdev->requested_height = height;
	hdev->requested_refresh_hz = refresh_hz;
	hdev->last_enable_ns = ktime_get_ns();
	hdev->output_enable_count++;
	request->session_id = hdev->session_id;
	owner_pid = hdev->owner_pid;
	mutex_unlock(&hdev->state_lock);

	request->width = width;
	request->height = height;
	request->refresh_hz = refresh_hz;
	request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_CONNECTED |
				 HERMES_KMS_SET_OUTPUT_RESULT_OWNER_ASSIGNED;

	/* Make the connector advertise the freshly requested mode. */
	hermes_kms_reprobe_modes(drm);

	hotplug_sent = hermes_kms_hotplug_event(drm);
	if (hotplug_sent)
		request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT;
	drm_info(drm,
		 "connected virtual output %ux%u@%u session=%llu owner_pid=%d hotplug_sent=%d\n",
		 width, height, refresh_hz,
		 (unsigned long long)request->session_id,
		 owner_pid ? owner_pid : -1,
		 hotplug_sent);

	return 0;
}

static void hermes_kms_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	bool disconnected = false;

	mutex_lock(&hdev->state_lock);
	if (hdev->owner_file == file) {
		hdev->output_enabled = false;
		hermes_kms_clear_owner_locked(hdev);
		hdev->last_disable_ns = ktime_get_ns();
		hdev->output_disable_count++;
		hdev->owner_close_disconnect_count++;
		disconnected = true;
	}
	mutex_unlock(&hdev->state_lock);

	if (!disconnected)
		return;

	hermes_kms_track_frame(hdev, NULL);
	hermes_kms_hotplug_event(drm);
	drm_info(drm, "disconnected virtual output after owner fd closed\n");
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
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_ACQUIRE_FRAME,
			  hermes_kms_ioctl_acquire_frame,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_GET_IDENTITY,
			  hermes_kms_ioctl_get_identity,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_WAIT_FRAME,
			  hermes_kms_ioctl_wait_frame,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_GET_METRICS,
			  hermes_kms_ioctl_get_metrics,
			  DRM_RENDER_ALLOW),
};

static const struct file_operations hermes_kms_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
};

#ifdef CONFIG_DEBUG_FS
/*
 * /sys/kernel/debug/dri/<n>/hermes_kms_stats — a human-readable dump of the
 * telemetry counters the ioctls already maintain, so the driver can be
 * inspected (pacing, export health, vblank overruns) without a userspace
 * client. Read-only; values are sampled under state_lock for consistency.
 */
static int hermes_kms_stats_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *drm = entry->dev;
	struct hermes_kms_device *hdev = to_hermes_kms(drm);

	mutex_lock(&hdev->state_lock);
	seq_printf(m, "output_enabled:        %d\n", hdev->output_enabled);
	seq_printf(m, "owner_pid:             %d\n", hdev->owner_pid);
	seq_printf(m, "session_id:            %llu\n", hdev->session_id);
	seq_printf(m, "requested_mode:        %ux%u@%u\n",
		   hdev->requested_width, hdev->requested_height,
		   hdev->requested_refresh_hz);
	seq_printf(m, "vblank_period_ns:      %lld\n",
		   ktime_to_ns(hdev->vblank_period));
	seq_printf(m, "vblank_count:          %llu\n",
		   READ_ONCE(hdev->vblank_count));
	seq_printf(m, "vblank_overrun_count:  %llu\n",
		   READ_ONCE(hdev->vblank_overrun_count));
	seq_printf(m, "frame_sequence:        %llu\n", hdev->frame_sequence);
	seq_printf(m, "frame_update_count:    %llu\n", hdev->frame_update_count);
	seq_printf(m, "acquire_count:         %llu\n", hdev->acquire_count);
	seq_printf(m, "acquire_no_frame:      %llu\n",
		   hdev->acquire_no_frame_count);
	seq_printf(m, "dmabuf_export_count:   %llu\n", hdev->dmabuf_export_count);
	seq_printf(m, "dmabuf_export_fail:    %llu\n",
		   hdev->dmabuf_export_fail_count);
	seq_printf(m, "sync_file_export:      %llu\n",
		   hdev->sync_file_export_count);
	seq_printf(m, "sync_file_export_fail: %llu\n",
		   hdev->sync_file_export_fail_count);
	seq_printf(m, "wait_count:            %llu\n", hdev->wait_count);
	seq_printf(m, "wait_timeout_count:    %llu\n", hdev->wait_timeout_count);
	seq_printf(m, "hotplug_event_count:   %llu\n", hdev->hotplug_event_count);
	seq_printf(m, "output_enable_count:   %llu\n", hdev->output_enable_count);
	seq_printf(m, "output_disable_count:  %llu\n", hdev->output_disable_count);
	mutex_unlock(&hdev->state_lock);
	return 0;
}

static const struct drm_debugfs_info hermes_kms_debugfs_list[] = {
	{ "hermes_kms_stats", hermes_kms_stats_show, 0, NULL },
};

static void hermes_kms_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_add_files(minor->dev, hermes_kms_debugfs_list,
			      ARRAY_SIZE(hermes_kms_debugfs_list));
}
#endif /* CONFIG_DEBUG_FS */

static const struct drm_driver hermes_kms_driver = {
	/*
	 * DRIVER_RENDER exposes a render node (/dev/dri/renderD*). The frame
	 * consumer (Hermes/Apollo) opens that node to call ACQUIRE_FRAME and
	 * friends, all of which are DRM_RENDER_ALLOW. A render node never holds
	 * DRM master, so the compositor (KWin/GNOME) can own the primary node
	 * (card*) and drive the modeset without an EBUSY conflict. This mirrors
	 * the EVDI model where the compositor owns the card and the consumer
	 * pulls frames through a side channel.
	 */
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC |
			   DRIVER_RENDER,
	.name = HERMES_KMS_DRIVER_NAME,
	.desc = HERMES_KMS_DRIVER_DESC,
	.major = HERMES_KMS_DRIVER_MAJOR,
	.minor = HERMES_KMS_DRIVER_MINOR,
	.fops = &hermes_kms_fops,
	.postclose = hermes_kms_postclose,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init = hermes_kms_debugfs_init,
#endif
	.ioctls = hermes_kms_ioctls,
	.num_ioctls = ARRAY_SIZE(hermes_kms_ioctls),
	DRM_GEM_SHMEM_DRIVER_OPS,
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
	/* Standard 256x256 cursor envelope so compositors size HW cursors. */
	drm->mode_config.cursor_width = 256;
	drm->mode_config.cursor_height = 256;
	drm->mode_config.funcs = &hermes_kms_mode_config_funcs;

	ret = drm_connector_init(drm, &hdev->connector,
				 &hermes_kms_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	drm_connector_helper_add(&hdev->connector,
				 &hermes_kms_connector_helper_funcs);

	/*
	 * Do not set the connector PATH property here. That property is
	 * reserved for DP-MST tunnelled connectors: drm_connector_set_path_property()
	 * returns -EINVAL on a plain DRM_MODE_CONNECTOR_VIRTUAL connector, and a
	 * PATH blob would mislead userspace (KScreen/KWin) into treating Hermes as
	 * an MST sink. The connector keeps its kernel-assigned "Virtual-N" name,
	 * which is what compositors enumerate. The friendly identity is reported
	 * separately via DRM_IOCTL_HERMES_KMS_GET_IDENTITY (output_name).
	 */

	hdev->connector.display_info.non_desktop = non_desktop;
	if (drm->mode_config.non_desktop_property)
		drm_object_attach_property(&hdev->connector.base,
					   drm->mode_config.non_desktop_property,
					   non_desktop ? 1 : 0);
	hdev->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				 DRM_CONNECTOR_POLL_DISCONNECT;

	/* Primary plane. */
	ret = drm_universal_plane_init(drm, &hdev->primary, 0,
				       &hermes_kms_plane_funcs,
				       hermes_kms_formats,
				       ARRAY_SIZE(hermes_kms_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&hdev->primary, &hermes_kms_plane_helper_funcs);

	/*
	 * Advertise FB_DAMAGE_CLIPS so the compositor can tell us which region
	 * changed each frame; we forward it to the capture consumer via
	 * ACQUIRE_FRAME's damage rect so only the dirty region is encoded.
	 */
	drm_plane_enable_fb_damage_clips(&hdev->primary);

	/* Cursor plane: lets the compositor offload pointer motion (no full
	 * recomposite per move). Consumed client-side, not blended into capture. */
	ret = drm_universal_plane_init(drm, &hdev->cursor, 0,
				       &hermes_kms_cursor_funcs,
				       hermes_kms_cursor_formats,
				       ARRAY_SIZE(hermes_kms_cursor_formats),
				       NULL, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&hdev->cursor, &hermes_kms_cursor_helper_funcs);

	/* CRTC driven by the software vblank timer. Use the managed variant to
	 * match vkms and pair with devm_drm_dev_alloc(). */
	drm_dbg_kms(drm, "primary plane type=%d (PRIMARY=%d) before crtc init\n",
		    hdev->primary.type, DRM_PLANE_TYPE_PRIMARY);
	ret = drmm_crtc_init_with_planes(drm, &hdev->crtc, &hdev->primary,
					 &hdev->cursor,
					 &hermes_kms_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&hdev->crtc, &hermes_kms_crtc_helper_funcs);

	/* Encoder linking the CRTC to the connector. */
	hdev->encoder.possible_crtcs = drm_crtc_mask(&hdev->crtc);
	ret = drm_encoder_init(drm, &hdev->encoder, &hermes_kms_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		return ret;

	ret = drm_connector_attach_encoder(&hdev->connector, &hdev->encoder);
	if (ret)
		return ret;

	/* One CRTC, with a software vblank timer driving its refresh. */
	ret = drm_vblank_init(drm, 1);
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
	mutex_init(&hdev->export_lock);
	init_waitqueue_head(&hdev->frame_wait);
	hrtimer_setup(&hdev->vblank_timer, hermes_kms_vblank_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hdev->next_session_id = 1;
	hermes_kms_init_output_state(drm, hdev);

	ret = hermes_kms_modeset_init(hdev);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_info(drm,
		 "registered Hermes-KMS virtual DRM device output=%s initial_enabled=%d hotplug_events=%d non_desktop=%d initial_mode=%ux%u@%u\n",
		 HERMES_KMS_OUTPUT_NAME,
		 initial_enabled,
		 hotplug_events,
		 non_desktop,
		 hdev->requested_width,
		 hdev->requested_height,
		 hdev->requested_refresh_hz);
	return 0;
}

static void hermes_kms_remove(struct platform_device *pdev)
{
	struct hermes_kms_device *hdev = platform_get_drvdata(pdev);
	struct drm_framebuffer *fb;

	mutex_lock(&hdev->state_lock);
	if (hdev->output_enabled) {
		hdev->output_enabled = false;
		hdev->last_disable_ns = ktime_get_ns();
		hdev->output_disable_count++;
	}
	hermes_kms_clear_owner_locked(hdev);
	mutex_unlock(&hdev->state_lock);
	hermes_kms_track_frame(hdev, NULL);

	hrtimer_cancel(&hdev->vblank_timer);

	drm_dev_unregister(&hdev->drm);
	drm_atomic_helper_shutdown(&hdev->drm);

	mutex_lock(&hdev->state_lock);
	fb = hdev->framebuffer;
	hdev->framebuffer = NULL;
	hermes_kms_clear_frame_locked(hdev);
	mutex_unlock(&hdev->state_lock);

	mutex_lock(&hdev->export_lock);
	hermes_kms_drop_export_cache_locked(hdev);
	mutex_unlock(&hdev->export_lock);

	if (fb)
		drm_framebuffer_put(fb);
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
MODULE_IMPORT_NS("DMA_BUF");
