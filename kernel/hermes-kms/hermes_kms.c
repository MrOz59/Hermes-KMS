// SPDX-License-Identifier: GPL-2.0
/*
 * Hermes-KMS - reusable DRM/KMS virtual display and capture driver.
 *
 * Policy and application identity intentionally stay in userspace. The kernel
 * interface exposes generic virtual outputs, fd-scoped ownership and opaque
 * session capabilities for trusted capture handoff.
 */

#include <linux/module.h>
#include <linux/build_bug.h>
#include <linux/compat.h>
#include <crypto/algapi.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/fdtable.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>
#include <linux/version.h>
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
#include <drm/drm_vblank_helper.h>

/*
 * Linux 7.2 renamed struct drm_atomic_state to struct drm_atomic_commit -- the
 * object was always one commit's worth of state, never the device's entire
 * state -- and retyped every atomic helper callback with it. Nothing else about
 * those callbacks changed, so the source stays on the current upstream name and
 * maps it back on older kernels.
 *
 * kbuild probes the target kernel's own drm_atomic.h and defines
 * HERMES_KMS_HAVE_DRM_ATOMIC_COMMIT, so a tree that backported the rename is
 * detected as well; LINUX_VERSION_CODE is the fallback for when that header
 * could not be read. Drop this block once 7.2 is the oldest kernel we build
 * against.
 *
 * Before 7.2 this also shadows the blocking commit function of the same name,
 * drm_atomic_commit(). The driver never calls it, and a call added later would
 * fail to build rather than do something else.
 */
#ifndef HERMES_KMS_HAVE_DRM_ATOMIC_COMMIT
#define HERMES_KMS_HAVE_DRM_ATOMIC_COMMIT \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0))
#endif

#if !HERMES_KMS_HAVE_DRM_ATOMIC_COMMIT
#define drm_atomic_commit drm_atomic_state
#endif

#define HERMES_KMS_DRIVER_NAME "hermes-kms"
#define HERMES_KMS_DRIVER_DESC "Hermes virtual KMS display"
#define HERMES_KMS_DRIVER_DATE "20260625"
#define HERMES_KMS_DRIVER_MAJOR 0
#define HERMES_KMS_DRIVER_MINOR 4
#define HERMES_KMS_DRIVER_PATCH 0
#define HERMES_KMS_OUTPUT_NAME_PREFIX "HERMES-"
#define HERMES_KMS_DEFAULT_OUTPUTS 1
#define HERMES_KMS_MAX_OUTPUTS 8
#define HERMES_KMS_DEFAULT_DEVICES 1
#define HERMES_KMS_MAX_DEVICES 8
#define HERMES_KMS_DEFAULT_SESSION_DEVICES 0
#define HERMES_KMS_MAX_SESSION_DEVICES 8
#define HERMES_KMS_MAX_REGISTERED_DEVICES \
	(1 + HERMES_KMS_MAX_SESSION_DEVICES)

#define HERMES_KMS_MIN_WIDTH 640
#define HERMES_KMS_MIN_HEIGHT 480
#define HERMES_KMS_MIN_FRAMEBUFFER_WIDTH 1
#define HERMES_KMS_MIN_FRAMEBUFFER_HEIGHT 1
#define HERMES_KMS_MAX_WIDTH 3840
#define HERMES_KMS_MAX_HEIGHT 2160
#define HERMES_KMS_DEFAULT_WIDTH 1920
#define HERMES_KMS_DEFAULT_HEIGHT 1080
#define HERMES_KMS_DEFAULT_REFRESH_HZ 60
#define HERMES_KMS_MAX_REFRESH_HZ 240
#define HERMES_KMS_OUTPUT_CHANGE_INTERVAL HZ
#define HERMES_KMS_OUTPUT_CHANGE_BURST 10

static bool initial_enabled;
static bool hotplug_events = true;
static bool non_desktop;
static bool insecure_legacy_unbound_access;
static unsigned int initial_width = HERMES_KMS_DEFAULT_WIDTH;
static unsigned int initial_height = HERMES_KMS_DEFAULT_HEIGHT;
static unsigned int initial_refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;
static unsigned int outputs = HERMES_KMS_DEFAULT_OUTPUTS;
static unsigned int devices = HERMES_KMS_DEFAULT_DEVICES;
static unsigned int session_devices = HERMES_KMS_DEFAULT_SESSION_DEVICES;
static unsigned int registered_device_count;

module_param(initial_enabled, bool, 0444);
MODULE_PARM_DESC(initial_enabled, "Initial virtual output state");
module_param(hotplug_events, bool, 0644);
MODULE_PARM_DESC(hotplug_events, "Emit DRM hotplug events when output state changes");
module_param(non_desktop, bool, 0444);
MODULE_PARM_DESC(non_desktop, "Mark connector as non-desktop. Default false so compositors can manage Hermes as a normal virtual monitor when connected");
module_param(insecure_legacy_unbound_access, bool, 0600);
MODULE_PARM_DESC(insecure_legacy_unbound_access,
		 "Allow pre-v11 unbound status/capture/wait/metrics access (unsafe, default false)");
module_param(initial_width, uint, 0444);
MODULE_PARM_DESC(initial_width, "Initial virtual output width");
module_param(initial_height, uint, 0444);
MODULE_PARM_DESC(initial_height, "Initial virtual output height");
module_param(initial_refresh_hz, uint, 0444);
MODULE_PARM_DESC(initial_refresh_hz, "Initial virtual output refresh rate");
module_param(outputs, uint, 0444);
MODULE_PARM_DESC(outputs, "Number of virtual outputs on the DRM device (1-8, default 1)");
module_param(devices, uint, 0444);
MODULE_PARM_DESC(devices, "Number of independent virtual DRM devices (1-8, default 1)");
module_param(session_devices, uint, 0444);
MODULE_PARM_DESC(session_devices, "Number of private session devices (0-8). When non-zero, also creates one seat0 host device");

struct hermes_kms_device;

struct hermes_kms_export_cache {
	struct drm_gem_object *obj[4];
	struct dma_buf *dmabuf[4];
};

struct hermes_kms_output {
	struct hermes_kms_device *hdev;
	unsigned int index;
	char output_name[HERMES_KMS_NAME_LEN];
	u8 edid[128];
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
	struct drm_framebuffer *cursor_framebuffer;
	struct drm_file *owner_file;
	/*
	 * Per-plane dma-buf export cache. drm_gem_prime_export() always
	 * allocates a fresh dma_buf, so re-exporting the same scanout BO every
	 * ACQUIRE_FRAME (60+ times per second) would force the consumer to
	 * re-import and re-map on every frame, defeating zero-copy. Cache the
	 * exported dma_buf keyed by the GEM object pointer and hand out a fresh
	 * fd referencing the cached dma_buf while the BO is unchanged. The cache
	 * holds a GEM reference as well as a dma-buf reference so pointer equality
	 * cannot suffer an ABA reuse after an imported GEM wrapper is destroyed.
	 * Protected by export_lock.
	 */
	struct mutex export_lock;
	struct hermes_kms_export_cache frame_export_cache;
	struct hermes_kms_export_cache cursor_export_cache;
	u64 access_token[2];
	atomic64_t authorization_generation;
	struct ratelimit_state output_change_ratelimit;
	u64 session_id;
	u64 next_session_id;
	/* Diagnostic status/debugfs value; never part of authorization. */
	pid_t owner_pid;
	bool output_enabled;
	bool output_transitioning;
	u32 requested_width;
	u32 requested_height;
	u32 requested_refresh_hz;
	atomic64_t frame_sequence;
	u64 framebuffer_generation;
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
	atomic64_t cursor_sequence;
	u64 cursor_image_sequence;
	u64 cursor_generation;
	u64 cursor_last_update_ns;
	s32 cursor_position_x;
	s32 cursor_position_y;
	s32 cursor_crtc_x;
	s32 cursor_crtc_y;
	u32 cursor_crtc_w;
	u32 cursor_crtc_h;
	u32 cursor_src_x;
	u32 cursor_src_y;
	u32 cursor_src_w;
	u32 cursor_src_h;
	s32 cursor_hotspot_x;
	s32 cursor_hotspot_y;
	bool cursor_position_valid;
	bool cursor_hotspot_valid;
	bool cursor_visible;
	u32 cursor_framebuffer_id;
	u32 cursor_width;
	u32 cursor_height;
	u32 cursor_format;
	u32 cursor_plane_count;
	u32 cursor_pitch[4];
	u32 cursor_offset[4];
	u64 cursor_modifier;
	u64 cursor_update_count;
	u64 cursor_acquire_count;
	u64 cursor_wait_count;
	/*
	 * Damage region computed from FB_DAMAGE_CLIPS and latched with the primary
	 * framebuffer at atomic_flush. damage_valid is cleared when the compositor
	 * provides no damage (treat the whole frame as dirty). Protected by
	 * state_lock.
	 */
	bool framebuffer_damage_valid;
	u32 framebuffer_damage_x1;
	u32 framebuffer_damage_y1;
	u32 framebuffer_damage_x2;
	u32 framebuffer_damage_y2;
	u64 frame_update_count;
	atomic64_t vblank_count;	   /* vblanks the software timer has fired */
	atomic64_t vblank_overrun_count;  /* timer ticks that fell behind */
	u64 last_vblank_callback_ns;
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

struct hermes_kms_device {
	struct drm_device drm;
	unsigned int device_index;
	unsigned int device_count;
	unsigned int device_role;
	unsigned int session_index;
	unsigned int session_device_count;
	unsigned int output_count;
	struct hermes_kms_output outputs[HERMES_KMS_MAX_OUTPUTS];
};

struct hermes_kms_file {
	struct mutex lock;
	unsigned int output_index;
	struct hermes_kms_output *bound_output;
	u64 bound_session_id;
	u64 bound_authorization_generation;
	atomic64_t binding_generation;
	struct hermes_kms_output *last_acquire_output;
	u64 last_acquire_session_id;
	u64 last_acquire_sequence;
};

/* Keep ioctl numbers/layouts identical on LP64 and newly compiled ILP32. */
static_assert(sizeof(struct drm_hermes_kms_status) == 208);
static_assert(offsetof(struct drm_hermes_kms_status, framebuffer_modifier) == 136);
static_assert(sizeof(struct drm_hermes_kms_acquire_frame) == 176);
static_assert(offsetof(struct drm_hermes_kms_acquire_frame, reserved) == 128);
static_assert(sizeof(struct drm_hermes_kms_metrics) == 312);
static_assert(sizeof(struct drm_hermes_kms_session_access) == 72);
static_assert(sizeof(struct drm_hermes_kms_acquire_cursor) == 224);
static_assert(offsetof(struct drm_hermes_kms_acquire_cursor, reserved) == 176);
static_assert(sizeof(struct drm_hermes_kms_wait_update) == 112);

static inline struct hermes_kms_device *to_hermes_kms(struct drm_device *drm)
{
	return container_of(drm, struct hermes_kms_device, drm);
}

static inline struct hermes_kms_output *
hermes_kms_output_for_context(struct hermes_kms_device *hdev,
			      struct hermes_kms_file *context)
{
	if (!context || context->output_index >= hdev->output_count)
		return &hdev->outputs[0];

	return &hdev->outputs[context->output_index];
}

static void
hermes_kms_reset_acquire_history_locked(struct hermes_kms_file *context)
{
	context->last_acquire_output = NULL;
	context->last_acquire_session_id = 0;
	context->last_acquire_sequence = 0;
}

/* Caller holds both context->lock and output->state_lock. */
static bool
hermes_kms_file_has_access_locked(struct hermes_kms_output *output,
				  struct drm_file *file,
				  struct hermes_kms_file *context)
{
	if (output->owner_file == file)
		return true;

	if (insecure_legacy_unbound_access)
		return true;

	return output->owner_file && context->bound_output == output &&
	       context->bound_session_id &&
	       context->bound_session_id == output->session_id &&
	       context->bound_authorization_generation ==
		atomic64_read(&output->authorization_generation);
}

/* Return with both locks held, in context -> output order. */
static int
hermes_kms_lock_scoped_output(struct hermes_kms_device *hdev,
			      struct drm_file *file, bool allow_public_idle,
			      struct hermes_kms_file **context_out,
			      struct hermes_kms_output **output_out)
{
	struct hermes_kms_file *context = file->driver_priv;
	struct hermes_kms_output *output;
	bool public_idle;

	if (!context)
		return -EINVAL;

	mutex_lock(&context->lock);
	output = hermes_kms_output_for_context(hdev, context);
	mutex_lock(&output->state_lock);
	public_idle = allow_public_idle && !output->owner_file &&
		      !output->output_enabled && !output->framebuffer;
	if (!public_idle &&
	    !hermes_kms_file_has_access_locked(output, file, context)) {
		mutex_unlock(&output->state_lock);
		mutex_unlock(&context->lock);
		return -EACCES;
	}

	*context_out = context;
	*output_out = output;
	return 0;
}

static void
hermes_kms_unlock_scoped_output(struct hermes_kms_file *context,
				struct hermes_kms_output *output)
{
	mutex_unlock(&output->state_lock);
	mutex_unlock(&context->lock);
}

/* Drop every cached dma-buf export. Caller must hold export_lock. */
static void
hermes_kms_drop_export_cache_locked(struct hermes_kms_export_cache *cache)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cache->dmabuf); i++) {
		if (cache->dmabuf[i]) {
			dma_buf_put(cache->dmabuf[i]);
			cache->dmabuf[i] = NULL;
		}
		if (cache->obj[i]) {
			drm_gem_object_put(cache->obj[i]);
			cache->obj[i] = NULL;
		}
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
 * (manufacturer "HRM", name "Hermes KMS") and range limits so the output is
 * treated as a normal monitor. The actual mode list is still generated
 * dynamically in get_modes() via CVT so arbitrary client geometries work; the
 * EDID's detailed timing is only a fallback/preferred hint. Checksum verified.
 *
 * The range limits are not a hint, though — userspace validates modes against
 * them, so they have to cover everything this driver accepts. They used to say
 * 75 Hz / 150 kHz / 300 MHz, which contradicted HERMES_KMS_MAX_REFRESH_HZ and
 * silently ruled out every mode above 75 Hz: a client asking for 1080p120 got a
 * connector that advertised the mode through CVT and then refused it here.
 * They now say 240 Hz / 255 kHz / 1200 MHz, matching what the driver allows.
 *
 * 255 kHz is the ceiling a 1.3 range descriptor can express without the 1.4
 * offset flags. That covers up to 1440p144; 4K above ~113 Hz needs more
 * horizontal rate than can be stated here, so it would still be filtered out.
 */
static const u8 hermes_kms_edid[128] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x22, 0x4d, 0x01, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x22, 0x01, 0x03, 0x80, 0x00, 0x00, 0x78,
	0x02, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
	0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x13, 0x2b, 0x21, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x48, 0x65, 0x72, 0x6d, 0x65, 0x73, 0x20,
	0x4b, 0x4d, 0x53, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17,
	0xf0, 0x0f, 0xff, 0x78, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6,
};

static bool hermes_kms_hotplug_event(struct hermes_kms_output *output)
{
	struct drm_device *drm = &output->hdev->drm;

	if (!hotplug_events)
		return false;

	mutex_lock(&output->state_lock);
	output->hotplug_event_count++;
	mutex_unlock(&output->state_lock);

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
static void hermes_kms_reprobe_modes(struct hermes_kms_output *output)
{
	struct drm_device *drm = &output->hdev->drm;

	mutex_lock(&drm->mode_config.mutex);
	drm_helper_probe_single_connector_modes(&output->connector,
						HERMES_KMS_MAX_WIDTH,
						HERMES_KMS_MAX_HEIGHT);
	mutex_unlock(&drm->mode_config.mutex);
}

static u64
hermes_kms_next_session_id_locked(struct hermes_kms_output *output)
{
	u64 session_id = output->next_session_id++;

	if (!output->next_session_id)
		output->next_session_id = 1;

	return session_id;
}

static void hermes_kms_clear_owner_locked(struct hermes_kms_output *output)
{
	bool had_authorization = output->owner_file || output->session_id ||
				 output->access_token[0] || output->access_token[1];

	output->owner_file = NULL;
	output->owner_pid = 0;
	output->session_id = 0;
	memzero_explicit(output->access_token, sizeof(output->access_token));
	if (had_authorization) {
		atomic64_inc(&output->authorization_generation);
		wake_up_interruptible(&output->frame_wait);
	}
}

/* Caller holds output->state_lock. */
static void hermes_kms_start_session_locked(struct hermes_kms_output *output,
					    struct drm_file *file)
{
	do {
		get_random_bytes(output->access_token,
				 sizeof(output->access_token));
	} while (!output->access_token[0] && !output->access_token[1]);

	output->owner_file = file;
	output->owner_pid = task_tgid_nr(current);
	output->session_id = hermes_kms_next_session_id_locked(output);
	atomic64_inc(&output->authorization_generation);
}

static void hermes_kms_clear_frame_locked(struct hermes_kms_output *output)
{
	output->framebuffer_id = 0;
	output->framebuffer_width = 0;
	output->framebuffer_height = 0;
	output->framebuffer_format = 0;
	output->framebuffer_plane_count = 0;
	memset(output->framebuffer_pitch, 0, sizeof(output->framebuffer_pitch));
	memset(output->framebuffer_offset, 0, sizeof(output->framebuffer_offset));
	output->framebuffer_modifier = 0;
}

static void hermes_kms_set_frame_metadata_locked(struct hermes_kms_output *output,
						 struct drm_framebuffer *fb)
{
	unsigned int i;
	unsigned int plane_count = 0;

	if (!fb) {
		hermes_kms_clear_frame_locked(output);
		return;
	}

	output->framebuffer_id = fb->base.id;
	output->framebuffer_width = fb->width;
	output->framebuffer_height = fb->height;
	output->framebuffer_format = fb->format->format;
	output->framebuffer_modifier = fb->modifier;

	if (fb->format->num_planes > ARRAY_SIZE(output->framebuffer_pitch))
		plane_count = ARRAY_SIZE(output->framebuffer_pitch);
	else
		plane_count = fb->format->num_planes;

	output->framebuffer_plane_count = plane_count;
	memset(output->framebuffer_pitch, 0, sizeof(output->framebuffer_pitch));
	memset(output->framebuffer_offset, 0, sizeof(output->framebuffer_offset));

	for (i = 0; i < plane_count; i++) {
		output->framebuffer_pitch[i] = fb->pitches[i];
		output->framebuffer_offset[i] = fb->offsets[i];
	}
}

static void hermes_kms_track_frame(struct hermes_kms_output *output,
				   struct drm_framebuffer *fb,
				   const struct drm_rect *damage,
				   bool notify)
{
	struct drm_device *drm = &output->hdev->drm;
	struct drm_framebuffer *old_fb;
	u32 old_fb_id;
	u64 sequence;
	bool log_frame_connected = false;
	bool log_frame_disconnected = false;

	if (fb)
		drm_framebuffer_get(fb);

	mutex_lock(&output->state_lock);
	/* Ignore late scanout commits while the connector is being handed over. */
	if (fb && (!output->output_enabled || output->output_transitioning)) {
		mutex_unlock(&output->state_lock);
		drm_framebuffer_put(fb);
		return;
	}
	old_fb = output->framebuffer;
	if (!fb && !old_fb) {
		mutex_unlock(&output->state_lock);
		/* Session handoff must invalidate exports even without a frame. */
		mutex_lock(&output->export_lock);
		hermes_kms_drop_export_cache_locked(&output->frame_export_cache);
		mutex_unlock(&output->export_lock);
		return;
	}
	old_fb_id = old_fb ? old_fb->base.id : 0;
	output->framebuffer = fb;
	if (old_fb != fb) {
		output->framebuffer_generation++;
		if (!output->framebuffer_generation)
			output->framebuffer_generation++;
	}
	sequence = atomic64_inc_return(&output->frame_sequence);
	output->frame_update_count++;
	output->last_update_ns = ktime_get_ns();
	hermes_kms_set_frame_metadata_locked(output, fb);
	if (fb && damage) {
		output->framebuffer_damage_valid = true;
		output->framebuffer_damage_x1 = max(damage->x1, 0);
		output->framebuffer_damage_y1 = max(damage->y1, 0);
		output->framebuffer_damage_x2 = max(damage->x2, 0);
		output->framebuffer_damage_y2 = max(damage->y2, 0);
	} else {
		output->framebuffer_damage_valid = false;
		output->framebuffer_damage_x1 = 0;
		output->framebuffer_damage_y1 = 0;
		output->framebuffer_damage_x2 = 0;
		output->framebuffer_damage_y2 = 0;
	}
	if (fb && !old_fb_id) {
		output->last_logged_framebuffer_id = fb->base.id;
		log_frame_connected = true;
	} else if (!fb && old_fb_id) {
		output->last_logged_framebuffer_id = 0;
		log_frame_disconnected = true;
	} else if (fb && output->last_logged_framebuffer_id != fb->base.id) {
		output->last_logged_framebuffer_id = fb->base.id;
		drm_dbg_kms(drm,
			    "%s scanout framebuffer changed: id=%u size=%ux%u format=0x%08x modifier=0x%016llx planes=%u sequence=%llu\n",
			    output->output_name, output->framebuffer_id,
			    output->framebuffer_width,
			    output->framebuffer_height,
			    output->framebuffer_format,
				    (unsigned long long)output->framebuffer_modifier,
				    output->framebuffer_plane_count,
				    (unsigned long long)sequence);
	}
	mutex_unlock(&output->state_lock);

	/*
	 * When the scanout framebuffer goes away (output disabled), drop the
	 * export cache so we do not pin the consumer's imported buffers. While
	 * a framebuffer is present the cache self-corrects: the export path
	 * re-exports whenever the compositor flips to a different BO. Taken
	 * outside state_lock to keep export_lock strictly below it.
	 */
	if (!fb) {
		mutex_lock(&output->export_lock);
		hermes_kms_drop_export_cache_locked(&output->frame_export_cache);
		mutex_unlock(&output->export_lock);
	}

	if (old_fb)
		drm_framebuffer_put(old_fb);

	if (log_frame_connected)
		drm_info(drm,
			 "%s first active scanout framebuffer: id=%u size=%ux%u format=0x%08x modifier=0x%016llx planes=%u sequence=%llu\n",
			 output->output_name, fb->base.id, fb->width, fb->height,
			 fb->format->format,
			 (unsigned long long)fb->modifier, fb->format->num_planes,
			 (unsigned long long)sequence);
	else if (log_frame_disconnected)
		drm_info(drm, "%s cleared active scanout framebuffer\n",
			 output->output_name);

	if (notify)
		wake_up_interruptible(&output->frame_wait);
}

static void hermes_kms_clear_cursor_metadata_locked(
	struct hermes_kms_output *output)
{
	output->cursor_position_x = 0;
	output->cursor_position_y = 0;
	output->cursor_crtc_x = 0;
	output->cursor_crtc_y = 0;
	output->cursor_crtc_w = 0;
	output->cursor_crtc_h = 0;
	output->cursor_src_x = 0;
	output->cursor_src_y = 0;
	output->cursor_src_w = 0;
	output->cursor_src_h = 0;
	output->cursor_hotspot_x = 0;
	output->cursor_hotspot_y = 0;
	output->cursor_position_valid = false;
	output->cursor_hotspot_valid = false;
	output->cursor_visible = false;
	output->cursor_framebuffer_id = 0;
	output->cursor_width = 0;
	output->cursor_height = 0;
	output->cursor_format = 0;
	output->cursor_plane_count = 0;
	memset(output->cursor_pitch, 0, sizeof(output->cursor_pitch));
	memset(output->cursor_offset, 0, sizeof(output->cursor_offset));
	output->cursor_modifier = 0;
}

static u32 hermes_kms_cursor_rect_coord(s32 coordinate)
{
	return coordinate > 0 ? (u32)coordinate : 0;
}

static u32 hermes_kms_cursor_rect_extent(s32 start, s32 end)
{
	s64 extent = (s64)end - (s64)start;

	return extent > 0 ? (u32)extent : 0;
}

/*
 * Latch one cursor-plane commit as an independent capture stream. @state is
 * NULL for an explicit session teardown. The stored framebuffer reference
 * keeps both metadata and later DMA-BUF export valid after atomic state cleanup.
 */
static void hermes_kms_track_cursor(struct hermes_kms_output *output,
				    const struct drm_plane_state *state,
				    bool notify)
{
	struct drm_framebuffer *fb = state ? state->fb : NULL;
	struct drm_framebuffer *old_fb;
	u64 sequence;
	u64 image_sequence;
	unsigned int plane_count = 0;
	unsigned int i;
	bool shape_changed;
	bool visible;

	if (fb)
		drm_framebuffer_get(fb);

	mutex_lock(&output->state_lock);
	if (fb && (!output->output_enabled || output->output_transitioning)) {
		mutex_unlock(&output->state_lock);
		drm_framebuffer_put(fb);
		return;
	}

	old_fb = output->cursor_framebuffer;
	if (!state && !old_fb && !output->cursor_position_valid) {
		mutex_unlock(&output->state_lock);
		mutex_lock(&output->export_lock);
		hermes_kms_drop_export_cache_locked(&output->cursor_export_cache);
		mutex_unlock(&output->export_lock);
		return;
	}

	if (fb)
		plane_count = min_t(unsigned int, fb->format->num_planes,
				    ARRAY_SIZE(output->cursor_pitch));
	shape_changed = old_fb != fb ||
			output->cursor_framebuffer_id != (fb ? fb->base.id : 0) ||
			output->cursor_width != (fb ? fb->width : 0) ||
			output->cursor_height != (fb ? fb->height : 0) ||
			output->cursor_format != (fb ? fb->format->format : 0) ||
			output->cursor_plane_count != plane_count ||
			output->cursor_modifier != (fb ? fb->modifier : 0);

	output->cursor_framebuffer = fb;
	if (shape_changed) {
		output->cursor_generation++;
		if (!output->cursor_generation)
			output->cursor_generation++;
	}
	sequence = atomic64_inc_return(&output->cursor_sequence);
	output->cursor_update_count++;
	output->cursor_last_update_ns = ktime_get_ns();

	/*
	 * GEM contents can be updated in place without changing the framebuffer
	 * identity or layout.  Conservatively make every committed cursor buffer
	 * a new image; consumers may still reuse the export cache underneath.
	 */
	if (shape_changed || (state && fb)) {
		output->cursor_image_sequence++;
		if (!output->cursor_image_sequence)
			output->cursor_image_sequence++;
	}

	if (!state) {
		hermes_kms_clear_cursor_metadata_locked(output);
	} else {
		output->cursor_position_valid = !!state->crtc;
		/*
		 * This is a universal KMS cursor plane, not a paravirtualized mouse
		 * device that requires DRIVER_CURSOR_HOTSPOT.  Compositors already
		 * account for their logical hotspot when positioning the plane, so its
		 * CRTC coordinates remain sufficient for capture.  Do not claim that
		 * the optional logical-pointer metadata was supplied by userspace.
		 */
		output->cursor_hotspot_valid = false;
		output->cursor_visible = fb && state->crtc && state->visible;
		output->cursor_position_x = state->crtc_x;
		output->cursor_position_y = state->crtc_y;
		output->cursor_crtc_x = state->dst.x1;
		output->cursor_crtc_y = state->dst.y1;
		output->cursor_crtc_w = hermes_kms_cursor_rect_extent(
			state->dst.x1, state->dst.x2);
		output->cursor_crtc_h = hermes_kms_cursor_rect_extent(
			state->dst.y1, state->dst.y2);
		output->cursor_src_x = hermes_kms_cursor_rect_coord(
			state->src.x1);
		output->cursor_src_y = hermes_kms_cursor_rect_coord(
			state->src.y1);
		output->cursor_src_w = hermes_kms_cursor_rect_extent(
			state->src.x1, state->src.x2);
		output->cursor_src_h = hermes_kms_cursor_rect_extent(
			state->src.y1, state->src.y2);
		output->cursor_hotspot_x = 0;
		output->cursor_hotspot_y = 0;
		output->cursor_framebuffer_id = fb ? fb->base.id : 0;
		output->cursor_width = fb ? fb->width : 0;
		output->cursor_height = fb ? fb->height : 0;
		output->cursor_format = fb ? fb->format->format : 0;
		output->cursor_plane_count = plane_count;
		memset(output->cursor_pitch, 0, sizeof(output->cursor_pitch));
		memset(output->cursor_offset, 0, sizeof(output->cursor_offset));
		for (i = 0; i < plane_count; i++) {
			output->cursor_pitch[i] = fb->pitches[i];
			output->cursor_offset[i] = fb->offsets[i];
		}
		output->cursor_modifier = fb ? fb->modifier : 0;
	}
	image_sequence = output->cursor_image_sequence;
	visible = output->cursor_visible;
	mutex_unlock(&output->state_lock);

	/*
	 * A different cursor image must not leave stale planes pinned in the
	 * export cache (notably when the new framebuffer has fewer planes).
	 * Export revalidation makes dropping after state_lock race-safe.
	 */
	if (shape_changed) {
		mutex_lock(&output->export_lock);
		hermes_kms_drop_export_cache_locked(&output->cursor_export_cache);
		mutex_unlock(&output->export_lock);
	}
	if (old_fb)
		drm_framebuffer_put(old_fb);

	drm_dbg_kms(&output->hdev->drm,
		    "%s cursor update sequence=%llu image=%llu visible=%d\n",
		    output->output_name, (unsigned long long)sequence,
		    (unsigned long long)image_sequence, visible);
	if (notify)
		wake_up_interruptible(&output->frame_wait);
}

static enum drm_connector_status
hermes_kms_connector_detect(struct drm_connector *connector, bool force)
{
	struct hermes_kms_output *output =
		container_of(connector, struct hermes_kms_output, connector);
	enum drm_connector_status status;

	mutex_lock(&output->state_lock);
	status = output->output_enabled ? connector_status_connected :
					  connector_status_disconnected;
	mutex_unlock(&output->state_lock);

	return status;
}

/*
 * drm_cvt_mode() rounds hdisplay down to an eight-pixel character-cell
 * boundary. That is appropriate for a physical CVT sink, but not for a
 * virtual monitor whose visible size must match a remote client's framebuffer
 * exactly (854 would otherwise become 848 and the encoded image would be
 * cropped/scaled).
 *
 * Keep CVT's blanking and sync widths, translate the horizontal timings by the
 * rounded delta, then recompute the pixel clock for the requested refresh.
 * The GEM dumb-buffer pitch remains independently aligned for DMA-BUF/VAAPI.
 */
static struct drm_display_mode *
hermes_kms_exact_cvt_mode(struct drm_device *drm, u32 width, u32 height,
			  u32 refresh_hz)
{
	struct drm_display_mode *mode;
	int horizontal_delta;

	mode = drm_cvt_mode(drm, width, height, refresh_hz,
			    false, false, false);
	if (!mode)
		return NULL;

	horizontal_delta = (int)width - mode->hdisplay;
	if (!horizontal_delta)
		return mode;

	mode->hdisplay += horizontal_delta;
	mode->hsync_start += horizontal_delta;
	mode->hsync_end += horizontal_delta;
	mode->htotal += horizontal_delta;
	mode->clock = DIV_ROUND_CLOSEST_ULL((u64)mode->htotal *
					   mode->vtotal * refresh_hz,
					   1000);
	drm_mode_set_name(mode);

	return mode;
}

static int hermes_kms_connector_get_modes(struct drm_connector *connector)
{
	int count = 0;
	struct hermes_kms_output *output =
		container_of(connector, struct hermes_kms_output, connector);
	const struct drm_edid *drm_edid;
	struct drm_display_mode *mode;
	u32 width;
	u32 height;
	u32 refresh_hz;

	mutex_lock(&output->state_lock);
	width = output->requested_width;
	height = output->requested_height;
	refresh_hz = output->requested_refresh_hz;
	mutex_unlock(&output->state_lock);

	/*
	 * Attach the synthetic EDID for identity. This makes compositors treat
	 * the connector as a normal monitor (name, manufacturer, range limits)
	 * instead of warning about a missing EDID. drm_edid_connector_update()
	 * also records the EDID so userspace can read it back. The EDID's own
	 * detailed-timing modes are added too, but the CVT mode below is marked
	 * preferred so the client's exact geometry still wins.
	 */
	drm_edid = drm_edid_alloc(output->edid, sizeof(output->edid));
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
	 * is selected by default. Preserve the visible width exactly: the generic
	 * CVT helper rounds it to eight-pixel cells, which is not valid for a
	 * remote framebuffer such as 854x480.
	 */
	if (width && height && refresh_hz) {
		mode = hermes_kms_exact_cvt_mode(connector->dev, width, height,
						refresh_hz);
		if (mode) {
			mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
			drm_mode_probed_add(connector, mode);
			count++;
			drm_info(connector->dev,
				 "get_modes added preferred exact mode requested=%ux%u active=%dx%d clock=%d vrefresh=%d name=%s\n",
				 width, height, mode->hdisplay, mode->vdisplay,
				 mode->clock,
				 drm_mode_vrefresh(mode), mode->name);
		} else {
			drm_warn(connector->dev,
				 "get_modes: exact CVT mode %ux%u@%u returned NULL\n",
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

static inline struct hermes_kms_output *
crtc_to_hermes_kms_output(struct drm_crtc *crtc)
{
	return container_of(crtc, struct hermes_kms_output, crtc);
}

/*
 * The DRM core owns and cancels the hrtimer without waiting for its callback
 * under vblank_time_lock. This hook only augments the core timer with metrics.
 */
static bool hermes_kms_handle_vblank_timeout(struct drm_crtc *crtc)
{
	struct hermes_kms_output *output =
		crtc_to_hermes_kms_output(crtc);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	u64 last_ns = READ_ONCE(output->last_vblank_callback_ns);
	u64 now_ns = ktime_get_ns();
	u64 elapsed_ns;
	u64 periods;
	u32 period_ns = READ_ONCE(vblank->framedur_ns);
	bool handled;

	if (last_ns && period_ns && now_ns > last_ns) {
		elapsed_ns = now_ns - last_ns;
		periods = div_u64(elapsed_ns, period_ns);
		if (periods > 1)
			atomic64_add(periods - 1,
				     &output->vblank_overrun_count);
	}
	WRITE_ONCE(output->last_vblank_callback_ns, now_ns);
	atomic64_inc(&output->vblank_count);

	handled = drm_crtc_handle_vblank(crtc);
	if (!handled)
		drm_err_ratelimited(&output->hdev->drm,
				    "%s failure handling vblank\n",
				    output->output_name);

	return handled;
}

static const struct drm_crtc_funcs hermes_kms_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

static enum drm_mode_status
hermes_kms_plane_mode_valid(const struct drm_display_mode *mode);

static int hermes_kms_crtc_atomic_check(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
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
					  struct drm_atomic_commit *state)
{
	struct hermes_kms_output *output =
		crtc_to_hermes_kms_output(crtc);

	mutex_lock(&output->state_lock);
	output->last_enable_ns = ktime_get_ns();
	WRITE_ONCE(output->last_vblank_callback_ns, 0);
	mutex_unlock(&output->state_lock);

	/*
	 * Compute the vblank timestamping constants (framedur_ns/linedur_ns)
	 * for the active mode before enabling vblank, so the vblank timer period
	 * and get_vblank_timestamp() are accurate. Required for timer-driven
	 * vblank; without it framedur_ns stays zero.
	 */
	drm_calc_timestamping_constants(crtc, &crtc->state->mode);

	drm_crtc_vblank_on(crtc);

	drm_info(&output->hdev->drm,
		 "%s enabled virtual display %ux%u@%d\n",
		 output->output_name,
		 crtc->state->mode.hdisplay,
		 crtc->state->mode.vdisplay,
		 drm_mode_vrefresh(&crtc->state->mode));
}

static void hermes_kms_crtc_atomic_disable(struct drm_crtc *crtc,
					   struct drm_atomic_commit *state)
{
	struct hermes_kms_output *output =
		crtc_to_hermes_kms_output(crtc);

	drm_crtc_vblank_off(crtc);

	mutex_lock(&output->state_lock);
	output->last_disable_ns = ktime_get_ns();
	mutex_unlock(&output->state_lock);
	hermes_kms_track_frame(output, NULL, NULL, false);
	hermes_kms_track_cursor(output, NULL, false);
	wake_up_interruptible(&output->frame_wait);

	drm_info(&output->hdev->drm, "%s disabled virtual display\n",
		 output->output_name);
}

static void hermes_kms_crtc_atomic_flush(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct hermes_kms_output *output =
		crtc_to_hermes_kms_output(crtc);
	struct drm_plane *plane = &output->primary;
	struct drm_plane_state *cursor_state;
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *new_plane_state;
	struct drm_rect damage;
	bool have_damage = false;

	new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (new_plane_state) {
		old_plane_state = drm_atomic_get_old_plane_state(state, plane);
		if (new_plane_state->fb && new_plane_state->crtc)
			have_damage = drm_atomic_helper_damage_merged(
				old_plane_state, new_plane_state, &damage);

		/*
		 * Latch framebuffer, damage and sequence together. A cursor-only
		 * commit has no new primary state and deliberately does not advance
		 * the capture sequence.
		 */
		hermes_kms_track_frame(output, new_plane_state->fb,
				       have_damage ? &damage : NULL, false);
	}
	cursor_state = drm_atomic_get_new_plane_state(state, &output->cursor);
	if (cursor_state)
		hermes_kms_track_cursor(output, cursor_state, false);
	if (new_plane_state || cursor_state)
		wake_up_interruptible(&output->frame_wait);

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
	.handle_vblank_timeout = hermes_kms_handle_vblank_timeout,
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
					 struct drm_atomic_commit *state)
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

/*
 * The virtual planes have no hardware registers to program. Their coherent
 * framebuffer/cursor snapshots are latched together from the completed atomic
 * state in the CRTC's atomic_flush callback. drm_atomic_helper_commit_planes()
 * still invokes the plane update hook for every active plane, so provide an
 * explicit no-op instead of leaving a callable helper slot NULL.
 */
static void hermes_kms_plane_atomic_noop(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	(void)plane;
	(void)state;
}

static const struct drm_plane_helper_funcs hermes_kms_plane_helper_funcs = {
	.atomic_check = hermes_kms_plane_atomic_check,
	.atomic_update = hermes_kms_plane_atomic_noop,
	.atomic_disable = hermes_kms_plane_atomic_noop,
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
 * consumer can detect the cursor plane separately and render it client-side,
 * so the cursor does not need to be blended into the captured primary plane.
 */
static int hermes_kms_cursor_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc)
		return 0;
	if (new_state->fb &&
	    (new_state->fb->width > plane->dev->mode_config.cursor_width ||
	     new_state->fb->height > plane->dev->mode_config.cursor_height))
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	/* Cursor may sit partly off-screen; the helper clips it without scaling. */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   true, true);
}

static const struct drm_plane_helper_funcs hermes_kms_cursor_helper_funcs = {
	.atomic_check = hermes_kms_cursor_atomic_check,
	.atomic_update = hermes_kms_plane_atomic_noop,
	.atomic_disable = hermes_kms_plane_atomic_noop,
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
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
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
			      HERMES_KMS_CAP_MULTI_OUTPUT |
			      HERMES_KMS_CAP_SESSION_TOKEN |
			      HERMES_KMS_CAP_CURSOR_CAPTURE |
			      HERMES_KMS_CAP_ZERO_COPY_TARGET |
		      HERMES_KMS_CAP_SYNC_FILE;
	if (hdev->device_count > 1)
		caps->flags |= HERMES_KMS_CAP_MULTI_DEVICE;
	if (session_devices)
		caps->flags |= HERMES_KMS_CAP_SESSION_DEVICE_POOL;
	caps->min_width = HERMES_KMS_MIN_WIDTH;
	caps->min_height = HERMES_KMS_MIN_HEIGHT;
	caps->max_width = HERMES_KMS_MAX_WIDTH;
	caps->max_height = HERMES_KMS_MAX_HEIGHT;
	caps->preferred_width = HERMES_KMS_DEFAULT_WIDTH;
	caps->preferred_height = HERMES_KMS_DEFAULT_HEIGHT;
	caps->max_refresh_hz = HERMES_KMS_MAX_REFRESH_HZ;
	caps->output_count = hdev->output_count;

	return 0;
}

static int hermes_kms_ioctl_select_output(struct drm_device *drm, void *data,
					  struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context = file->driver_priv;
	struct drm_hermes_kms_select_output *request = data;
	struct hermes_kms_output *current_output;
	struct hermes_kms_output *old_bound;
	struct hermes_kms_output *selected;
	unsigned int output_index = request->output_index;
	int ret = 0;

	if (!context)
		return -EINVAL;
	if (request->flags ||
	    memchr_inv(request->reserved, 0, sizeof(request->reserved)) ||
	    output_index >= hdev->output_count)
		return -EINVAL;

	mutex_lock(&context->lock);
	current_output = hermes_kms_output_for_context(hdev, context);
	selected = &hdev->outputs[output_index];
	if (current_output == selected) {
		mutex_lock(&selected->state_lock);
		if (selected->owner_file &&
		    !hermes_kms_file_has_access_locked(selected, file, context))
			ret = -EACCES;
		mutex_unlock(&selected->state_lock);
		if (ret)
			goto unlock_context;
		goto fill_response;
	}

	mutex_lock(&current_output->state_lock);
	if (current_output->owner_file == file) {
		ret = -EBUSY;
		goto unlock_current;
	}
	mutex_unlock(&current_output->state_lock);

	/* BIND, rather than SELECT_OUTPUT, authorizes an active foreign session. */
	mutex_lock(&selected->state_lock);
	if (selected->owner_file && !insecure_legacy_unbound_access)
		ret = -EACCES;
	mutex_unlock(&selected->state_lock);
	if (ret)
		goto unlock_context;

	old_bound = context->bound_output;
	context->output_index = output_index;
	context->bound_output = NULL;
	context->bound_session_id = 0;
	context->bound_authorization_generation = 0;
	hermes_kms_reset_acquire_history_locked(context);
	atomic64_inc(&context->binding_generation);
	/* A waiter may be authorized as the owner without bound_output set. */
	wake_up_interruptible(&current_output->frame_wait);
	if (old_bound && old_bound != current_output)
		wake_up_interruptible(&old_bound->frame_wait);

fill_response:
	memset(request, 0, sizeof(*request));
	request->output_index = output_index;
	request->selected_output_index = output_index;
	request->output_count = hdev->output_count;
	strscpy(request->output_name, selected->output_name,
		sizeof(request->output_name));
	mutex_unlock(&context->lock);
	return 0;

unlock_current:
	mutex_unlock(&current_output->state_lock);
unlock_context:
	mutex_unlock(&context->lock);
	return ret;
}

static int hermes_kms_ioctl_get_status(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context = file->driver_priv;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_status *status = data;
	struct drm_crtc_state *crtc_state;
	bool public_idle;
	int ret;

	memset(status, 0, sizeof(*status));

	if (!context)
		return -EINVAL;

	/*
	 * Keep selection, authorization and CRTC state in one linearizable
	 * snapshot. In particular, an idle/public status call must not pass its
	 * access check and then observe a replacement session's active mode.
	 * The CRTC -> state_lock order matches the atomic modeset callbacks.
	 */
	mutex_lock(&context->lock);
	output = hermes_kms_output_for_context(hdev, context);
	ret = drm_modeset_lock(&output->crtc.mutex, NULL);
	if (ret) {
		mutex_unlock(&context->lock);
		return ret;
	}
	mutex_lock(&output->state_lock);
	public_idle = !output->owner_file && !output->output_enabled &&
		      !output->framebuffer;
	if (!public_idle &&
	    !hermes_kms_file_has_access_locked(output, file, context)) {
		ret = -EACCES;
		goto unlock_status;
	}

	if (output->output_enabled)
		status->flags |= HERMES_KMS_STATUS_OUTPUT_ENABLED |
				 HERMES_KMS_STATUS_CONNECTED;
	if (hotplug_events)
		status->flags |= HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED;
	if (output->owner_file)
		status->flags |= HERMES_KMS_STATUS_SESSION_OWNED;

	status->requested_width = output->requested_width;
	status->requested_height = output->requested_height;
	status->requested_refresh_hz = output->requested_refresh_hz;
	status->frame_sequence = atomic64_read(&output->frame_sequence);
	status->last_update_ns = output->last_update_ns;
	status->last_enable_ns = output->last_enable_ns;
	status->last_disable_ns = output->last_disable_ns;
	status->framebuffer_id = output->framebuffer_id;
	status->framebuffer_width = output->framebuffer_width;
	status->framebuffer_height = output->framebuffer_height;
	status->framebuffer_format = output->framebuffer_format;
	status->framebuffer_plane_count = output->framebuffer_plane_count;
	memcpy(status->framebuffer_pitch, output->framebuffer_pitch,
	       sizeof(status->framebuffer_pitch));
	memcpy(status->framebuffer_offset, output->framebuffer_offset,
	       sizeof(status->framebuffer_offset));
	status->framebuffer_modifier = output->framebuffer_modifier;
	status->session_id = output->session_id;
	status->owner_pid = output->owner_pid ? output->owner_pid : -1;
	if (output->framebuffer_id)
		status->flags |= HERMES_KMS_STATUS_FRAME_VALID;
	if (output->framebuffer)
		status->flags |= HERMES_KMS_STATUS_DMABUF_EXPORT_READY;

	status->connector_id = output->connector.base.id;
	status->crtc_id = output->crtc.base.id;
	status->plane_id = output->primary.base.id;
	status->encoder_id = output->encoder.base.id;

	crtc_state = output->crtc.state;
	if (crtc_state && crtc_state->enable) {
		status->flags |= HERMES_KMS_STATUS_SCANOUT_ACTIVE;
		status->active_width = crtc_state->mode.hdisplay;
		status->active_height = crtc_state->mode.vdisplay;
		status->active_refresh_hz = drm_mode_vrefresh(&crtc_state->mode);
	}

unlock_status:
	mutex_unlock(&output->state_lock);
	drm_modeset_unlock(&output->crtc.mutex);
	mutex_unlock(&context->lock);

	return ret;
}

static int hermes_kms_ioctl_get_metrics(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_metrics *metrics = data;
	int ret;

	memset(metrics, 0, sizeof(*metrics));

	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &output);
	if (ret)
		return ret;
	metrics->frame_sequence = atomic64_read(&output->frame_sequence);
	metrics->frame_update_count = output->frame_update_count;
	metrics->acquire_count = output->acquire_count;
	metrics->acquire_no_frame_count = output->acquire_no_frame_count;
	metrics->dmabuf_export_count = output->dmabuf_export_count;
	metrics->dmabuf_export_fail_count = output->dmabuf_export_fail_count;
	metrics->sync_file_export_count = output->sync_file_export_count;
	metrics->sync_file_export_fail_count = output->sync_file_export_fail_count;
	metrics->wait_count = output->wait_count;
	metrics->wait_ready_count = output->wait_ready_count;
	metrics->wait_timeout_count = output->wait_timeout_count;
	metrics->wait_interrupted_count = output->wait_interrupted_count;
	metrics->output_enable_count = output->output_enable_count;
	metrics->output_disable_count = output->output_disable_count;
	metrics->hotplug_event_count = output->hotplug_event_count;
	metrics->owner_close_disconnect_count =
		output->owner_close_disconnect_count;
	metrics->last_update_ns = output->last_update_ns;
	metrics->last_acquire_ns = output->last_acquire_ns;
	metrics->last_wait_start_ns = output->last_wait_start_ns;
	metrics->last_wait_end_ns = output->last_wait_end_ns;
	metrics->last_wait_duration_ns = output->last_wait_duration_ns;
	metrics->last_dmabuf_export_ns = output->last_dmabuf_export_ns;
	metrics->last_sync_file_export_ns = output->last_sync_file_export_ns;
	metrics->vblank_count = atomic64_read(&output->vblank_count);
	metrics->vblank_overrun_count =
		atomic64_read(&output->vblank_overrun_count);
	hermes_kms_unlock_scoped_output(context, output);

	return 0;
}

static int hermes_kms_ioctl_get_identity(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context = file->driver_priv;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_identity *identity = data;
	const char *connector_name;

	if (!context)
		return -EINVAL;

	mutex_lock(&context->lock);
	output = hermes_kms_output_for_context(hdev, context);
	connector_name = output->connector.name;

	memset(identity, 0, sizeof(*identity));
	strscpy(identity->driver_name, HERMES_KMS_DRIVER_NAME,
		sizeof(identity->driver_name));
	strscpy(identity->output_name, output->output_name,
		sizeof(identity->output_name));
	if (connector_name)
		strscpy(identity->connector_name, connector_name,
			sizeof(identity->connector_name));

	identity->connector_id = output->connector.base.id;
	identity->crtc_id = output->crtc.base.id;
	identity->plane_id = output->primary.base.id;
	identity->encoder_id = output->encoder.base.id;
	identity->output_index = output->index;
	identity->output_count = hdev->output_count;
	identity->device_index = hdev->device_index;
	identity->device_count = hdev->device_count;
	identity->device_role = hdev->device_role;
	identity->session_index = hdev->session_index;
	identity->session_device_count = hdev->session_device_count;
	identity->cursor_plane_id = output->cursor.base.id;
	mutex_unlock(&context->lock);

	return 0;
}

static u64 hermes_kms_status_flags_locked(struct hermes_kms_output *output)
{
	u64 flags = 0;

	if (output->output_enabled)
		flags |= HERMES_KMS_STATUS_OUTPUT_ENABLED |
			 HERMES_KMS_STATUS_CONNECTED;
	if (hotplug_events)
		flags |= HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED;
	if (output->owner_file)
		flags |= HERMES_KMS_STATUS_SESSION_OWNED;
	if (output->framebuffer_id)
		flags |= HERMES_KMS_STATUS_FRAME_VALID;
	if (output->framebuffer)
		flags |= HERMES_KMS_STATUS_DMABUF_EXPORT_READY;

	return flags;
}

static void hermes_kms_fill_wait_frame_locked(struct hermes_kms_output *output,
					      struct drm_hermes_kms_wait_frame *wait)
{
	wait->sequence = atomic64_read(&output->frame_sequence);
	wait->timestamp_ns = output->last_update_ns;
	wait->status_flags = hermes_kms_status_flags_locked(output);
	if (output->framebuffer)
		wait->flags |= HERMES_KMS_WAIT_FRAME_READY;
}

static int hermes_kms_finish_wait_timeout(
	struct hermes_kms_device *hdev, struct drm_file *file,
	struct hermes_kms_output *expected_output, u64 binding_generation,
	u64 authorization_generation, u64 start_ns, int timeout_error)
{
	struct hermes_kms_file *context;
	struct hermes_kms_output *selected;
	u64 end_ns = ktime_get_ns();
	int ret;

	/* Revocation wins over a timeout, as promised by the public UAPI. */
	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &selected);
	if (ret)
		return ret;
	if (selected != expected_output ||
	    atomic64_read(&context->binding_generation) != binding_generation ||
	    atomic64_read(&expected_output->authorization_generation) !=
		authorization_generation) {
		hermes_kms_unlock_scoped_output(context, selected);
		return -EACCES;
	}

	expected_output->wait_timeout_count++;
	expected_output->last_wait_end_ns = end_ns;
	expected_output->last_wait_duration_ns = end_ns - start_ns;
	hermes_kms_unlock_scoped_output(context, expected_output);
	return timeout_error;
}

static int hermes_kms_ioctl_wait_frame(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context;
	struct hermes_kms_output *output;
	struct hermes_kms_output *selected;
	struct drm_hermes_kms_wait_frame *wait = data;
	u64 after_sequence = wait->after_sequence;
	u64 binding_generation;
	u64 authorization_generation;
	u32 timeout_ms = wait->timeout_ms;
	u64 start_ns = ktime_get_ns();
	u64 end_ns;
	long timeout;
	int ret;

	if (wait->flags || wait->reserved0 ||
	    memchr_inv(wait->reserved, 0, sizeof(wait->reserved)))
		return -EINVAL;

	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &output);
	if (ret)
		return ret;
	output->wait_count++;
	output->last_wait_start_ns = start_ns;
	binding_generation = atomic64_read(&context->binding_generation);
	authorization_generation =
		atomic64_read(&output->authorization_generation);
	hermes_kms_unlock_scoped_output(context, output);

	if (atomic64_read(&output->frame_sequence) <= after_sequence) {
		if (!timeout_ms)
			return hermes_kms_finish_wait_timeout(
				hdev, file, output, binding_generation,
				authorization_generation, start_ns, -EAGAIN);

		timeout = wait_event_interruptible_timeout(
			output->frame_wait,
			atomic64_read(&output->frame_sequence) > after_sequence ||
			atomic64_read(&context->binding_generation) !=
				binding_generation ||
			atomic64_read(&output->authorization_generation) !=
				authorization_generation,
			msecs_to_jiffies(timeout_ms));
		if (timeout < 0) {
			mutex_lock(&output->state_lock);
			output->wait_interrupted_count++;
			output->last_wait_end_ns = ktime_get_ns();
			output->last_wait_duration_ns =
				output->last_wait_end_ns - start_ns;
			mutex_unlock(&output->state_lock);
			return timeout;
		}
		if (!timeout)
			return hermes_kms_finish_wait_timeout(
				hdev, file, output, binding_generation,
				authorization_generation, start_ns, -ETIMEDOUT);
	}

	end_ns = ktime_get_ns();
	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &selected);
	if (ret)
		return ret;
	if (selected != output ||
	    atomic64_read(&context->binding_generation) != binding_generation ||
	    atomic64_read(&output->authorization_generation) !=
		authorization_generation) {
		hermes_kms_unlock_scoped_output(context, selected);
		return -EACCES;
	}

	memset(wait, 0, sizeof(*wait));
	output->wait_ready_count++;
	output->last_wait_end_ns = end_ns;
	output->last_wait_duration_ns = end_ns - start_ns;
	hermes_kms_fill_wait_frame_locked(output, wait);
	hermes_kms_unlock_scoped_output(context, output);

	return 0;
}

static void hermes_kms_fill_wait_update_locked(
	struct hermes_kms_output *output,
	struct drm_hermes_kms_wait_update *wait,
	u64 after_frame_sequence, u64 after_cursor_sequence)
{
	wait->frame_sequence = atomic64_read(&output->frame_sequence);
	wait->cursor_sequence = atomic64_read(&output->cursor_sequence);
	wait->frame_timestamp_ns = output->last_update_ns;
	wait->cursor_timestamp_ns = output->cursor_last_update_ns;
	wait->status_flags = hermes_kms_status_flags_locked(output);
	if (wait->frame_sequence > after_frame_sequence)
		wait->flags |= HERMES_KMS_WAIT_UPDATE_FRAME_READY;
	if (wait->cursor_sequence > after_cursor_sequence)
		wait->flags |= HERMES_KMS_WAIT_UPDATE_CURSOR_READY;
}

static int hermes_kms_ioctl_wait_update(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context;
	struct hermes_kms_output *output;
	struct hermes_kms_output *selected;
	struct drm_hermes_kms_wait_update *wait = data;
	u64 after_frame_sequence = wait->after_frame_sequence;
	u64 after_cursor_sequence = wait->after_cursor_sequence;
	u64 binding_generation;
	u64 authorization_generation;
	u32 timeout_ms = wait->timeout_ms;
	u64 start_ns = ktime_get_ns();
	u64 end_ns;
	long timeout;
	int ret;

	if (wait->flags || wait->reserved0 ||
	    memchr_inv(wait->reserved, 0, sizeof(wait->reserved)))
		return -EINVAL;

	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &output);
	if (ret)
		return ret;
	output->wait_count++;
	output->cursor_wait_count++;
	output->last_wait_start_ns = start_ns;
	binding_generation = atomic64_read(&context->binding_generation);
	authorization_generation =
		atomic64_read(&output->authorization_generation);
	hermes_kms_unlock_scoped_output(context, output);

	if (atomic64_read(&output->frame_sequence) <= after_frame_sequence &&
	    atomic64_read(&output->cursor_sequence) <= after_cursor_sequence) {
		if (!timeout_ms)
			return hermes_kms_finish_wait_timeout(
				hdev, file, output, binding_generation,
				authorization_generation, start_ns, -EAGAIN);

		timeout = wait_event_interruptible_timeout(
			output->frame_wait,
			atomic64_read(&output->frame_sequence) >
				after_frame_sequence ||
			atomic64_read(&output->cursor_sequence) >
				after_cursor_sequence ||
			atomic64_read(&context->binding_generation) !=
				binding_generation ||
			atomic64_read(&output->authorization_generation) !=
				authorization_generation,
			msecs_to_jiffies(timeout_ms));
		if (timeout < 0) {
			mutex_lock(&output->state_lock);
			output->wait_interrupted_count++;
			output->last_wait_end_ns = ktime_get_ns();
			output->last_wait_duration_ns =
				output->last_wait_end_ns - start_ns;
			mutex_unlock(&output->state_lock);
			return timeout;
		}
		if (!timeout)
			return hermes_kms_finish_wait_timeout(
				hdev, file, output, binding_generation,
				authorization_generation, start_ns, -ETIMEDOUT);
	}

	end_ns = ktime_get_ns();
	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &selected);
	if (ret)
		return ret;
	if (selected != output ||
	    atomic64_read(&context->binding_generation) != binding_generation ||
	    atomic64_read(&output->authorization_generation) !=
		authorization_generation) {
		hermes_kms_unlock_scoped_output(context, selected);
		return -EACCES;
	}

	memset(wait, 0, sizeof(*wait));
	output->wait_ready_count++;
	output->last_wait_end_ns = end_ns;
	output->last_wait_duration_ns = end_ns - start_ns;
	hermes_kms_fill_wait_update_locked(output, wait, after_frame_sequence,
					   after_cursor_sequence);
	hermes_kms_unlock_scoped_output(context, output);

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
hermes_kms_get_plane_dmabuf_locked(struct hermes_kms_export_cache *cache,
				   struct drm_framebuffer *fb,
				   unsigned int index)
{
	struct drm_gem_object *obj;
	struct dma_buf *dmabuf;

	obj = drm_gem_fb_get_obj(fb, index);
	if (!obj)
		return ERR_PTR(-EINVAL);

	if (cache->obj[index] == obj && cache->dmabuf[index]) {
		get_dma_buf(cache->dmabuf[index]);
		return cache->dmabuf[index];
	}

	/*
	 * The scanout object is not always one we allocated. A compositor that
	 * renders on the real GPU can import that buffer into this device and
	 * scan out of it directly, in which case gem_prime_import gives us a
	 * shmem object with no shmem file behind it: import_attach is set and
	 * obj->filp is NULL.
	 *
	 * Re-exporting such an object with drm_gem_prime_export() hands the
	 * consumer a dma_buf whose pages cannot be pinned. The importing driver
	 * discovers this only once it attaches - drm_gem_map_attach() ->
	 * drm_gem_shmem_pin_locked() warns on drm_gem_is_imported(), then
	 * drm_gem_get_pages() rejects the missing filp with -EINVAL - and the
	 * failure surfaces far away as EGL_BAD_ALLOC out of eglCreateImage().
	 *
	 * Hand back the dma_buf it was imported from instead. That is the
	 * buffer the GPU already understands, and the consumer imports it the
	 * same way the compositor did.
	 */
	if (obj->import_attach) {
		dmabuf = obj->import_attach->dmabuf;
		get_dma_buf(dmabuf);
	} else {
		dmabuf = drm_gem_prime_export(obj, O_RDWR);
		if (IS_ERR(dmabuf))
			return dmabuf;
	}

	/* Replace the cache entry; the cache holds one reference to each object. */
	if (cache->dmabuf[index])
		dma_buf_put(cache->dmabuf[index]);
	if (cache->obj[index])
		drm_gem_object_put(cache->obj[index]);
	get_dma_buf(dmabuf);
	drm_gem_object_get(obj);
	cache->dmabuf[index] = dmabuf;
	cache->obj[index] = obj;

	return dmabuf;
}

static int hermes_kms_export_frame_dmabufs(struct hermes_kms_output *output,
					   struct drm_framebuffer *fb,
					   u64 framebuffer_generation,
					   u64 authorization_generation,
					   u64 frame_sequence,
					   struct drm_hermes_kms_acquire_frame *frame)
{
	unsigned int i;
	int ret = 0;

	/*
	 * Hold the state lock through cache lookup and fd installation. Otherwise
	 * an acquire of an old framebuffer can repopulate the cache after a newer
	 * flip/disconnect dropped it. The generation also catches an ABA where the
	 * same framebuffer pointer is presented again by a later commit.
	 */
	mutex_lock(&output->state_lock);
	if (atomic64_read(&output->authorization_generation) !=
	    authorization_generation) {
		mutex_unlock(&output->state_lock);
		return -EACCES;
	}
	if (output->framebuffer != fb ||
	    output->framebuffer_generation != framebuffer_generation ||
	    atomic64_read(&output->frame_sequence) != frame_sequence) {
		mutex_unlock(&output->state_lock);
		return -ESTALE;
	}
	mutex_lock(&output->export_lock);
	for (i = 0; i < frame->plane_count; i++) {
		struct dma_buf *dmabuf;
		int fd;

		dmabuf = hermes_kms_get_plane_dmabuf_locked(
			&output->frame_export_cache, fb, i);
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
	mutex_unlock(&output->export_lock);
	mutex_unlock(&output->state_lock);

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
hermes_kms_frame_fence(struct drm_framebuffer *fb, u64 timestamp_ns)
{
	struct drm_gem_object *obj;
	struct dma_fence *fence = NULL;

	obj = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	if (obj && obj->resv) {
		int ret;

		ret = dma_resv_get_singleton(obj->resv, DMA_RESV_USAGE_WRITE,
					     &fence);
		if (ret)
			return ERR_PTR(ret);
	}

	if (!fence)
		fence = dma_fence_allocate_private_stub(
			ns_to_ktime(timestamp_ns));

	return fence;
}

static int hermes_kms_export_sync_file(struct hermes_kms_output *output,
				       struct drm_framebuffer *fb,
				       u64 buffer_generation,
				       u64 authorization_generation,
				       bool cursor_buffer,
				       u64 sequence,
				       u64 image_sequence,
				       u64 timestamp_ns, int *sync_file_fd)
{
	struct dma_fence *fence;
	struct sync_file *sync_file;
	int fd;
	int ret = 0;

	/*
	 * Keep revocation and buffer replacement from racing fd installation.
	 * Without this check a caller could pass the DMA-BUF revalidation, lose
	 * its session, and still receive a new sync_file for the old buffer.
	 */
	mutex_lock(&output->state_lock);
	if (atomic64_read(&output->authorization_generation) !=
	    authorization_generation) {
		ret = -EACCES;
		goto out_unlock;
	}
	if (cursor_buffer) {
		if (output->cursor_framebuffer != fb ||
		    output->cursor_generation != buffer_generation ||
		    atomic64_read(&output->cursor_sequence) != sequence ||
		    output->cursor_image_sequence != image_sequence) {
			ret = -ESTALE;
			goto out_unlock;
		}
	} else if (output->framebuffer != fb ||
		   output->framebuffer_generation != buffer_generation ||
		   atomic64_read(&output->frame_sequence) != sequence) {
		ret = -ESTALE;
		goto out_unlock;
	}

	fence = hermes_kms_frame_fence(fb, timestamp_ns);
	if (IS_ERR(fence)) {
		ret = PTR_ERR(fence);
		goto out_unlock;
	}
	if (!fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	sync_file = sync_file_create(fence);
	dma_fence_put(fence);
	if (!sync_file) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sync_file->file);
		ret = fd;
		goto out_unlock;
	}

	fd_install(fd, sync_file->file);
	*sync_file_fd = fd;

out_unlock:
	mutex_unlock(&output->state_lock);
	return ret;
}

static int hermes_kms_ioctl_acquire_frame(struct drm_device *drm, void *data,
					  struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_acquire_frame *frame = data;
	struct drm_framebuffer *fb;
	u64 requested_flags = frame->flags;
	u64 framebuffer_generation;
	u64 authorization_generation;
	u64 sequence;
	u64 session_id;
	bool damage_valid = false;
	int ret = 0;

	if (requested_flags & ~(HERMES_KMS_FRAME_REQUEST_DMABUF |
				HERMES_KMS_FRAME_REQUEST_SYNC_FILE))
		return -EINVAL;
	if (frame->reserved0 ||
	    memchr_inv(frame->reserved, 0, sizeof(frame->reserved)))
		return -EINVAL;

	memset(frame, 0, sizeof(*frame));
	hermes_kms_init_invalid_frame_fds(frame);

	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &output);
	if (ret)
		return ret;
	output->acquire_count++;
	output->last_acquire_ns = ktime_get_ns();
	if (!output->framebuffer) {
		output->acquire_no_frame_count++;
		if (output->acquire_no_frame_log_count < 5) {
			output->acquire_no_frame_log_count++;
			drm_info(drm,
				 "%s ACQUIRE_FRAME has no framebuffer yet: output_enabled=%d owner_pid=%d session=%llu sequence=%llu\n",
				 output->output_name, output->output_enabled,
					 output->owner_pid ? output->owner_pid : -1,
					 (unsigned long long)output->session_id,
					 (unsigned long long)atomic64_read(
						 &output->frame_sequence));
		} else {
			drm_dbg_kms_ratelimited(drm,
						"%s ACQUIRE_FRAME has no framebuffer yet: output_enabled=%d owner_pid=%d session=%llu sequence=%llu\n",
						output->output_name,
						output->output_enabled,
						output->owner_pid ? output->owner_pid : -1,
						(unsigned long long)output->session_id,
						(unsigned long long)atomic64_read(
							&output->frame_sequence));
		}
		hermes_kms_unlock_scoped_output(context, output);
		return -ENODATA;
	}

	fb = output->framebuffer;
	drm_framebuffer_get(fb);
	framebuffer_generation = output->framebuffer_generation;
	authorization_generation =
		atomic64_read(&output->authorization_generation);
	sequence = atomic64_read(&output->frame_sequence);
	session_id = output->session_id;
	frame->flags = HERMES_KMS_FRAME_METADATA_VALID;
	frame->sequence = sequence;
	frame->timestamp_ns = output->last_update_ns;
	frame->modifier = output->framebuffer_modifier;
	frame->framebuffer_id = output->framebuffer_id;
	frame->width = output->framebuffer_width;
	frame->height = output->framebuffer_height;
	frame->format = output->framebuffer_format;
	frame->plane_count = output->framebuffer_plane_count;
	memcpy(frame->pitch, output->framebuffer_pitch, sizeof(frame->pitch));
	memcpy(frame->offset, output->framebuffer_offset, sizeof(frame->offset));
	/*
	 * Damage describes one transition only. A repeated acquire is an empty
	 * change; a consecutive new sequence may use the latched clip. The first
	 * acquire, a session/output change, or any sequence gap is full-frame.
	 */
	if (context->last_acquire_output == output &&
	    context->last_acquire_session_id == session_id) {
		if (context->last_acquire_sequence == sequence) {
			damage_valid = true;
		} else if (sequence > context->last_acquire_sequence &&
			   sequence - context->last_acquire_sequence == 1 &&
			   output->framebuffer_damage_valid) {
			damage_valid = true;
			frame->damage_x1 = output->framebuffer_damage_x1;
			frame->damage_y1 = output->framebuffer_damage_y1;
			frame->damage_x2 = output->framebuffer_damage_x2;
			frame->damage_y2 = output->framebuffer_damage_y2;
		}
	}
	if (damage_valid)
		frame->flags |= HERMES_KMS_FRAME_DAMAGE_VALID;
	mutex_unlock(&output->state_lock);

	if (requested_flags & HERMES_KMS_FRAME_REQUEST_DMABUF) {
		ret = hermes_kms_export_frame_dmabufs(output, fb,
						     framebuffer_generation,
						     authorization_generation,
						     sequence,
						     frame);
		mutex_lock(&output->state_lock);
		if (ret) {
			output->dmabuf_export_fail_count++;
			mutex_unlock(&output->state_lock);
			hermes_kms_close_frame_fds(frame);
		} else {
			output->dmabuf_export_count++;
			output->last_dmabuf_export_ns = ktime_get_ns();
			mutex_unlock(&output->state_lock);
		}
		if (ret)
			goto out_put_fb;
	} else {
		frame->flags |= HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED;
	}

	if (requested_flags & HERMES_KMS_FRAME_REQUEST_SYNC_FILE) {
		ret = hermes_kms_export_sync_file(
			output, fb, framebuffer_generation,
			authorization_generation, false, sequence, 0,
			frame->timestamp_ns,
			&frame->sync_file_fd);
		mutex_lock(&output->state_lock);
		if (ret) {
			output->sync_file_export_fail_count++;
			mutex_unlock(&output->state_lock);
			hermes_kms_close_frame_fds(frame);
		} else {
			output->sync_file_export_count++;
			output->last_sync_file_export_ns = ktime_get_ns();
			frame->flags |= HERMES_KMS_FRAME_SYNC_FILE_VALID;
			mutex_unlock(&output->state_lock);
		}
	}
	if (!ret) {
		context->last_acquire_output = output;
		context->last_acquire_session_id = session_id;
		context->last_acquire_sequence = sequence;
	}

out_put_fb:
	drm_framebuffer_put(fb);
	mutex_unlock(&context->lock);
	return ret;
}

static void hermes_kms_init_invalid_cursor_fds(
	struct drm_hermes_kms_acquire_cursor *cursor)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cursor->dma_buf_fd); i++)
		cursor->dma_buf_fd[i] = -1;
	cursor->sync_file_fd = -1;
}

static void hermes_kms_close_cursor_fds(
	struct drm_hermes_kms_acquire_cursor *cursor)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cursor->dma_buf_fd); i++) {
		if (cursor->dma_buf_fd[i] >= 0) {
			close_fd(cursor->dma_buf_fd[i]);
			cursor->dma_buf_fd[i] = -1;
		}
	}
	if (cursor->sync_file_fd >= 0) {
		close_fd(cursor->sync_file_fd);
		cursor->sync_file_fd = -1;
	}
}

static int hermes_kms_export_cursor_dmabufs(
	struct hermes_kms_output *output, struct drm_framebuffer *fb,
	u64 cursor_generation, u64 authorization_generation,
	u64 cursor_sequence, u64 cursor_image_sequence,
	struct drm_hermes_kms_acquire_cursor *cursor)
{
	unsigned int i;
	int ret = 0;

	mutex_lock(&output->state_lock);
	if (atomic64_read(&output->authorization_generation) !=
	    authorization_generation) {
		mutex_unlock(&output->state_lock);
		return -EACCES;
	}
	if (output->cursor_framebuffer != fb ||
	    output->cursor_generation != cursor_generation ||
	    atomic64_read(&output->cursor_sequence) != cursor_sequence ||
	    output->cursor_image_sequence != cursor_image_sequence) {
		mutex_unlock(&output->state_lock);
		return -ESTALE;
	}

	mutex_lock(&output->export_lock);
	for (i = 0; i < cursor->plane_count; i++) {
		struct dma_buf *dmabuf;
		int fd;

		dmabuf = hermes_kms_get_plane_dmabuf_locked(
			&output->cursor_export_cache, fb, i);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			break;
		}
		fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (fd < 0) {
			dma_buf_put(dmabuf);
			ret = fd;
			break;
		}
		cursor->dma_buf_fd[i] = fd;
	}
	mutex_unlock(&output->export_lock);
	mutex_unlock(&output->state_lock);

	if (!ret)
		cursor->flags |= HERMES_KMS_CURSOR_DMABUF_VALID;
	return ret;
}

static int hermes_kms_ioctl_acquire_cursor(struct drm_device *drm, void *data,
					   struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_acquire_cursor *cursor = data;
	struct drm_framebuffer *fb = NULL;
	u64 requested_flags = cursor->flags;
	u64 cursor_generation = 0;
	u64 authorization_generation = 0;
	int ret;

	if (requested_flags & ~(HERMES_KMS_CURSOR_REQUEST_DMABUF |
				HERMES_KMS_CURSOR_REQUEST_SYNC_FILE))
		return -EINVAL;
	if (cursor->reserved0 || cursor->reserved_alignment ||
	    memchr_inv(cursor->reserved, 0, sizeof(cursor->reserved)))
		return -EINVAL;

	memset(cursor, 0, sizeof(*cursor));
	hermes_kms_init_invalid_cursor_fds(cursor);

	ret = hermes_kms_lock_scoped_output(hdev, file, false,
					    &context, &output);
	if (ret)
		return ret;
	output->cursor_acquire_count++;
	cursor->flags = HERMES_KMS_CURSOR_METADATA_VALID;
	cursor->sequence = atomic64_read(&output->cursor_sequence);
	cursor->image_sequence = output->cursor_image_sequence;
	cursor->timestamp_ns = output->cursor_last_update_ns;
	cursor->session_id = output->session_id;
	cursor->position_x = output->cursor_position_x;
	cursor->position_y = output->cursor_position_y;
	cursor->crtc_x = output->cursor_crtc_x;
	cursor->crtc_y = output->cursor_crtc_y;
	cursor->crtc_w = output->cursor_crtc_w;
	cursor->crtc_h = output->cursor_crtc_h;
	cursor->src_x = output->cursor_src_x;
	cursor->src_y = output->cursor_src_y;
	cursor->src_w = output->cursor_src_w;
	cursor->src_h = output->cursor_src_h;
	cursor->hotspot_x = output->cursor_hotspot_x;
	cursor->hotspot_y = output->cursor_hotspot_y;
	if (output->cursor_position_valid)
		cursor->flags |= HERMES_KMS_CURSOR_POSITION_VALID |
				 HERMES_KMS_CURSOR_GEOMETRY_VALID;
	if (output->cursor_hotspot_valid)
		cursor->flags |= HERMES_KMS_CURSOR_HOTSPOT_VALID;
	if (output->cursor_visible)
		cursor->flags |= HERMES_KMS_CURSOR_VISIBLE;
	if (output->cursor_framebuffer) {
		fb = output->cursor_framebuffer;
		drm_framebuffer_get(fb);
		cursor_generation = output->cursor_generation;
		authorization_generation =
			atomic64_read(&output->authorization_generation);
		cursor->flags |= HERMES_KMS_CURSOR_BUFFER_VALID;
		cursor->modifier = output->cursor_modifier;
		cursor->framebuffer_id = output->cursor_framebuffer_id;
		cursor->width = output->cursor_width;
		cursor->height = output->cursor_height;
		cursor->format = output->cursor_format;
		cursor->plane_count = output->cursor_plane_count;
		memcpy(cursor->pitch, output->cursor_pitch,
		       sizeof(cursor->pitch));
		memcpy(cursor->offset, output->cursor_offset,
		       sizeof(cursor->offset));
	}
	mutex_unlock(&output->state_lock);

	if (!fb)
		goto out_unlock_context;

	if (requested_flags & HERMES_KMS_CURSOR_REQUEST_DMABUF) {
		ret = hermes_kms_export_cursor_dmabufs(
			output, fb, cursor_generation, authorization_generation,
			cursor->sequence, cursor->image_sequence,
			cursor);
		mutex_lock(&output->state_lock);
		if (ret) {
			output->dmabuf_export_fail_count++;
			mutex_unlock(&output->state_lock);
			hermes_kms_close_cursor_fds(cursor);
			goto out_put_fb;
		}
		output->dmabuf_export_count++;
		output->last_dmabuf_export_ns = ktime_get_ns();
		mutex_unlock(&output->state_lock);
	}
	if (requested_flags & HERMES_KMS_CURSOR_REQUEST_SYNC_FILE) {
		ret = hermes_kms_export_sync_file(
			output, fb, cursor_generation, authorization_generation,
			true, cursor->sequence, cursor->image_sequence,
			cursor->timestamp_ns, &cursor->sync_file_fd);
		mutex_lock(&output->state_lock);
		if (ret) {
			output->sync_file_export_fail_count++;
			mutex_unlock(&output->state_lock);
			hermes_kms_close_cursor_fds(cursor);
			goto out_put_fb;
		}
		output->sync_file_export_count++;
		output->last_sync_file_export_ns = ktime_get_ns();
		cursor->flags |= HERMES_KMS_CURSOR_SYNC_FILE_VALID;
		mutex_unlock(&output->state_lock);
	}

out_put_fb:
	drm_framebuffer_put(fb);
out_unlock_context:
	mutex_unlock(&context->lock);
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
					 struct hermes_kms_output *output)
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

	output->output_enabled = initial_enabled;
	output->requested_width = width;
	output->requested_height = height;
	output->requested_refresh_hz = refresh_hz;
}

static int hermes_kms_ioctl_set_output(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context = file->driver_priv;
	struct hermes_kms_output *output;
	struct drm_hermes_kms_set_output *request = data;
	u32 width = request->width;
	u32 height = request->height;
	u32 refresh_hz = request->refresh_hz;
	pid_t owner_pid;
	bool hotplug_sent = false;
	bool mode_changed;
	bool owner_assigned;
	bool session_transition = false;
	bool state_changed;
	bool was_enabled;

	if (!context)
		return -EINVAL;
	if (request->flags || request->enabled > 1)
		return -EINVAL;

	request->result_flags = 0;
	request->session_id = 0;
	mutex_lock(&context->lock);
	output = hermes_kms_output_for_context(hdev, context);

	if (!request->enabled) {
		u64 session_id;

		mutex_lock(&output->state_lock);
		if (output->output_transitioning) {
			mutex_unlock(&output->state_lock);
			mutex_unlock(&context->lock);
			return -EAGAIN;
		}
		if (!output->owner_file && !output->output_enabled) {
			/* A disconnected output is already in the requested state. */
			request->width = output->requested_width;
			request->height = output->requested_height;
			request->refresh_hz = output->requested_refresh_hz;
			mutex_unlock(&output->state_lock);
			mutex_unlock(&context->lock);
			return 0;
		}
		/*
		 * An output exposed by initial_enabled=1 has no owner or session.
		 * Let an ordinary control fd disconnect that legacy state; a live
		 * session remains protected by the owner_file check below.
		 */
		if (output->owner_file && output->owner_file != file) {
			mutex_unlock(&output->state_lock);
			mutex_unlock(&context->lock);
			return -EACCES;
		}

		session_id = output->session_id;
		owner_pid = output->owner_pid;
		was_enabled = output->output_enabled;
		if (was_enabled &&
		    !__ratelimit(&output->output_change_ratelimit)) {
			mutex_unlock(&output->state_lock);
			mutex_unlock(&context->lock);
			return -EAGAIN;
		}
		output->output_transitioning = true;
		output->output_enabled = false;
		hermes_kms_clear_owner_locked(output);
		output->last_disable_ns = ktime_get_ns();
		if (was_enabled)
			output->output_disable_count++;
		mutex_unlock(&output->state_lock);
		hermes_kms_track_frame(output, NULL, NULL, false);
		hermes_kms_track_cursor(output, NULL, false);
		wake_up_interruptible(&output->frame_wait);
		if (was_enabled)
			hotplug_sent = hermes_kms_hotplug_event(output);
		mutex_lock(&output->state_lock);
		output->output_transitioning = false;
		mutex_unlock(&output->state_lock);
		request->session_id = session_id;
		if (hotplug_sent)
			request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT;
		drm_dbg_kms(drm,
			    "%s disconnected virtual output session=%llu owner_pid=%d hotplug_sent=%d\n",
			    output->output_name, (unsigned long long)session_id,
			    owner_pid ? owner_pid : -1, hotplug_sent);
		mutex_unlock(&context->lock);
		return 0;
	}

	if (!width)
		width = HERMES_KMS_DEFAULT_WIDTH;
	if (!height)
		height = HERMES_KMS_DEFAULT_HEIGHT;
	if (!refresh_hz)
		refresh_hz = HERMES_KMS_DEFAULT_REFRESH_HZ;

	if (!hermes_kms_valid_requested_mode(width, height, refresh_hz)) {
		mutex_unlock(&context->lock);
		return -EINVAL;
	}

	mutex_lock(&output->state_lock);
	if (output->output_transitioning) {
		mutex_unlock(&output->state_lock);
		mutex_unlock(&context->lock);
		return -EAGAIN;
	}
	if (output->owner_file && output->owner_file != file) {
		mutex_unlock(&output->state_lock);
		mutex_unlock(&context->lock);
		return -EBUSY;
	}

	was_enabled = output->output_enabled;
	mode_changed = output->requested_width != width ||
		       output->requested_height != height ||
		       output->requested_refresh_hz != refresh_hz;
	owner_assigned = output->owner_file != file || !output->session_id;
	state_changed = !was_enabled || mode_changed || owner_assigned;
	if (state_changed &&
	    !__ratelimit(&output->output_change_ratelimit)) {
		mutex_unlock(&output->state_lock);
		mutex_unlock(&context->lock);
		return -EAGAIN;
	}
	if (owner_assigned) {
		output->output_transitioning = true;
		session_transition = true;
	}
	output->output_enabled = true;
	if (owner_assigned) {
		hermes_kms_start_session_locked(output, file);
		hermes_kms_reset_acquire_history_locked(context);
	}
	output->requested_width = width;
	output->requested_height = height;
	output->requested_refresh_hz = refresh_hz;
	if (state_changed)
		output->last_enable_ns = ktime_get_ns();
	if (!was_enabled)
		output->output_enable_count++;
	request->session_id = output->session_id;
	owner_pid = output->owner_pid;
	mutex_unlock(&output->state_lock);

	request->width = width;
	request->height = height;
	request->refresh_hz = refresh_hz;
	request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_CONNECTED |
		HERMES_KMS_SET_OUTPUT_RESULT_OWNER_ASSIGNED;
	if (session_transition) {
		hermes_kms_track_frame(output, NULL, NULL, false);
		hermes_kms_track_cursor(output, NULL, false);
		wake_up_interruptible(&output->frame_wait);
	}

	/* Exact repeats are successful without reprobe, hotplug, counters or logs. */
	if (!state_changed) {
		if (session_transition) {
			mutex_lock(&output->state_lock);
			output->output_transitioning = false;
			mutex_unlock(&output->state_lock);
		}
		mutex_unlock(&context->lock);
		return 0;
	}

	/* Make the connector advertise the freshly requested mode. */
	if (mode_changed)
		hermes_kms_reprobe_modes(output);

	hotplug_sent = hermes_kms_hotplug_event(output);
	if (session_transition) {
		mutex_lock(&output->state_lock);
		output->output_transitioning = false;
		mutex_unlock(&output->state_lock);
	}
	if (hotplug_sent)
		request->result_flags |= HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT;
	drm_dbg_kms(drm,
		    "%s connected virtual output %ux%u@%u session=%llu owner_pid=%d hotplug_sent=%d\n",
		    output->output_name, width, height, refresh_hz,
		    (unsigned long long)request->session_id,
		    owner_pid ? owner_pid : -1, hotplug_sent);
	mutex_unlock(&context->lock);

	return 0;
}

static int hermes_kms_ioctl_session_access(struct drm_device *drm, void *data,
					   struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	struct hermes_kms_file *context = file->driver_priv;
	struct drm_hermes_kms_session_access *request = data;
	struct hermes_kms_output *current_output;
	struct hermes_kms_output *old_bound;
	struct hermes_kms_output *output;
	u64 token[2];
	u64 session_id;
	u32 operation = request->operation;
	u32 output_index = request->output_index;
	int ret = 0;

	if (!context)
		return -EINVAL;
	if (request->flags || request->result_flags ||
	    memchr_inv(request->reserved, 0, sizeof(request->reserved)))
		return -EINVAL;

	switch (operation) {
	case HERMES_KMS_SESSION_ACCESS_GET_TOKEN:
		if (request->token[0] || request->token[1] ||
		    request->session_id || request->output_index)
			return -EINVAL;

		mutex_lock(&context->lock);
		output = hermes_kms_output_for_context(hdev, context);
		mutex_lock(&output->state_lock);
		if (output->owner_file != file || !output->session_id) {
			ret = -EACCES;
			goto get_token_unlock;
		}
		token[0] = output->access_token[0];
		token[1] = output->access_token[1];
		session_id = output->session_id;
		output_index = output->index;
get_token_unlock:
		mutex_unlock(&output->state_lock);
		mutex_unlock(&context->lock);
		if (ret)
			return ret;

		memset(request, 0, sizeof(*request));
		request->token[0] = token[0];
		request->token[1] = token[1];
		request->session_id = session_id;
		request->operation = operation;
		request->output_index = output_index;
		request->result_flags =
			HERMES_KMS_SESSION_ACCESS_RESULT_TOKEN_VALID;
		memzero_explicit(token, sizeof(token));
		return 0;

	case HERMES_KMS_SESSION_ACCESS_BIND:
		if (!request->session_id ||
		    (!request->token[0] && !request->token[1]) ||
		    output_index >= hdev->output_count)
			return -EINVAL;
		token[0] = request->token[0];
		token[1] = request->token[1];
		session_id = request->session_id;

		mutex_lock(&context->lock);
		current_output = hermes_kms_output_for_context(hdev, context);
		output = &hdev->outputs[output_index];
		if (current_output != output) {
			mutex_lock(&current_output->state_lock);
			if (current_output->owner_file == file)
				ret = -EBUSY;
			mutex_unlock(&current_output->state_lock);
			if (ret)
				goto bind_unlock_context;
		}

		mutex_lock(&output->state_lock);
		if (!output->owner_file || !output->output_enabled ||
		    output->session_id != session_id ||
		    crypto_memneq(token, output->access_token, sizeof(token))) {
			ret = -EACCES;
			goto bind_unlock_output;
		}

		/*
		 * Publish selection and authorization while the verified session is
		 * still locked. This is the BIND linearization point: revocation can
		 * happen wholly before it (and fail) or after it (and invalidate the
		 * recorded generation), never in between validation and publication.
		 */
		old_bound = context->bound_output;
		context->output_index = output_index;
		context->bound_output = output;
		context->bound_session_id = session_id;
		context->bound_authorization_generation =
			atomic64_read(&output->authorization_generation);
		hermes_kms_reset_acquire_history_locked(context);
		atomic64_inc(&context->binding_generation);
bind_unlock_output:
		mutex_unlock(&output->state_lock);
bind_unlock_context:
		mutex_unlock(&context->lock);
		if (ret) {
			memzero_explicit(token, sizeof(token));
			return ret;
		}
		/* Wake owner/unbound waiters too; they may have no old_bound. */
		wake_up_interruptible(&current_output->frame_wait);
		if (old_bound && old_bound != current_output)
			wake_up_interruptible(&old_bound->frame_wait);

		memset(request, 0, sizeof(*request));
		request->session_id = session_id;
		request->operation = operation;
		request->output_index = output_index;
		request->result_flags =
			HERMES_KMS_SESSION_ACCESS_RESULT_BOUND;
		memzero_explicit(token, sizeof(token));
		return 0;

	case HERMES_KMS_SESSION_ACCESS_UNBIND:
		if (request->token[0] || request->token[1] ||
		    request->session_id || request->output_index)
			return -EINVAL;

		mutex_lock(&context->lock);
		current_output = hermes_kms_output_for_context(hdev, context);
		old_bound = context->bound_output;
		output_index = context->output_index;
		context->bound_output = NULL;
		context->bound_session_id = 0;
		context->bound_authorization_generation = 0;
		hermes_kms_reset_acquire_history_locked(context);
		atomic64_inc(&context->binding_generation);
		mutex_unlock(&context->lock);
		/* The owner is authorized without bound_output, but may be waiting. */
		wake_up_interruptible(&current_output->frame_wait);
		if (old_bound && old_bound != current_output)
			wake_up_interruptible(&old_bound->frame_wait);

		memset(request, 0, sizeof(*request));
		request->operation = operation;
		request->output_index = output_index;
		return 0;
	default:
		return -EINVAL;
	}
}

static int hermes_kms_open(struct drm_device *drm, struct drm_file *file)
{
	struct hermes_kms_file *context;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	/* UAPI <= 7 compatibility: an unselected fd controls HERMES-1. */
	mutex_init(&context->lock);
	atomic64_set(&context->binding_generation, 1);
	context->output_index = 0;
	file->driver_priv = context;
	return 0;
}

static void hermes_kms_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct hermes_kms_device *hdev = to_hermes_kms(drm);
	unsigned int i;

	for (i = 0; i < hdev->output_count; i++) {
		struct hermes_kms_output *output = &hdev->outputs[i];
		bool disconnected = false;

		mutex_lock(&output->state_lock);
		if (output->owner_file == file) {
			output->output_transitioning = true;
			output->output_enabled = false;
			hermes_kms_clear_owner_locked(output);
			output->last_disable_ns = ktime_get_ns();
			output->output_disable_count++;
			output->owner_close_disconnect_count++;
			disconnected = true;
		}
		mutex_unlock(&output->state_lock);

		if (!disconnected)
			continue;

		hermes_kms_track_frame(output, NULL, NULL, false);
		hermes_kms_track_cursor(output, NULL, false);
		wake_up_interruptible(&output->frame_wait);
		hermes_kms_hotplug_event(output);
		mutex_lock(&output->state_lock);
		output->output_transitioning = false;
		mutex_unlock(&output->state_lock);
		drm_info(drm,
			 "%s disconnected after owner fd closed\n",
			 output->output_name);
	}

	kfree(file->driver_priv);
	file->driver_priv = NULL;
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
	DRM_IOCTL_DEF_DRV(HERMES_KMS_SELECT_OUTPUT,
			  hermes_kms_ioctl_select_output,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_SESSION_ACCESS,
			  hermes_kms_ioctl_session_access,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_ACQUIRE_CURSOR,
			  hermes_kms_ioctl_acquire_cursor,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HERMES_KMS_WAIT_UPDATE,
			  hermes_kms_ioctl_wait_update,
			  DRM_RENDER_ALLOW),
};

#ifdef CONFIG_COMPAT
/*
 * UAPI <= 10 used plain __u64. On i386 that gave these two structures a
 * different size (and therefore a different ioctl command) from LP64. Keep
 * accepting already-built 32-bit clients while UAPI 11's __aligned_u64 makes
 * newly compiled ILP32 and LP64 layouts identical.
 */
struct drm_hermes_kms_status_v10_compat {
	__u64 flags;
	__u64 frame_sequence;
	__u64 last_update_ns;
	__u64 last_enable_ns;
	__u64 last_disable_ns;
	__u32 connector_id;
	__u32 crtc_id;
	__u32 plane_id;
	__u32 encoder_id;
	__u32 requested_width;
	__u32 requested_height;
	__u32 requested_refresh_hz;
	__u32 active_width;
	__u32 active_height;
	__u32 active_refresh_hz;
	__u32 framebuffer_id;
	__u32 framebuffer_width;
	__u32 framebuffer_height;
	__u32 framebuffer_format;
	__u32 framebuffer_plane_count;
	__u32 framebuffer_pitch[4];
	__u32 framebuffer_offset[4];
	__u64 framebuffer_modifier;
	__u64 session_id;
	__s32 owner_pid;
	__u32 reserved0;
	__u64 reserved[6];
} __packed;

struct drm_hermes_kms_acquire_frame_v10_compat {
	__u64 flags;
	__u64 sequence;
	__u64 timestamp_ns;
	__u64 modifier;
	__u32 framebuffer_id;
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 plane_count;
	__u32 pitch[4];
	__u32 offset[4];
	__s32 dma_buf_fd[4];
	__s32 sync_file_fd;
	__u32 reserved0;
	__u32 damage_x1;
	__u32 damage_y1;
	__u32 damage_x2;
	__u32 damage_y2;
	__u64 reserved[6];
} __packed;

static_assert(sizeof(struct drm_hermes_kms_status_v10_compat) == 204);
static_assert(offsetof(struct drm_hermes_kms_status_v10_compat,
		       framebuffer_modifier) == 132);
static_assert(sizeof(struct drm_hermes_kms_acquire_frame_v10_compat) == 172);
static_assert(offsetof(struct drm_hermes_kms_acquire_frame_v10_compat,
		       reserved) == 124);

#define DRM_IOCTL_HERMES_KMS_GET_STATUS_V10_COMPAT \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_STATUS, \
		struct drm_hermes_kms_status_v10_compat)
#define DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME_V10_COMPAT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_ACQUIRE_FRAME, \
		 struct drm_hermes_kms_acquire_frame_v10_compat)

static long hermes_kms_compat_get_status(struct file *filp, unsigned long arg)
{
	struct drm_hermes_kms_status_v10_compat status32 = { };
	struct drm_hermes_kms_status status = { };
	int ret;

	ret = drm_ioctl_kernel(filp, hermes_kms_ioctl_get_status, &status,
			       DRM_RENDER_ALLOW);
	if (ret)
		return ret;

	/* Everything before LP64's old implicit pad has an identical layout. */
	memcpy(&status32, &status,
	       offsetof(struct drm_hermes_kms_status, reserved_alignment));
	status32.framebuffer_modifier = status.framebuffer_modifier;
	status32.session_id = status.session_id;
	status32.owner_pid = status.owner_pid;
	status32.reserved0 = status.reserved0;
	memcpy(status32.reserved, status.reserved, sizeof(status32.reserved));

	if (copy_to_user(compat_ptr(arg), &status32, sizeof(status32)))
		return -EFAULT;
	return 0;
}

static long hermes_kms_compat_acquire_frame(struct file *filp,
					    unsigned long arg)
{
	struct drm_hermes_kms_acquire_frame_v10_compat frame32;
	struct drm_hermes_kms_acquire_frame frame = { };
	unsigned int i;
	int ret;

	if (copy_from_user(&frame32, compat_ptr(arg), sizeof(frame32)))
		return -EFAULT;

	frame.flags = frame32.flags;
	frame.reserved0 = frame32.reserved0;
	for (i = 0; i < ARRAY_SIZE(frame.reserved); i++)
		frame.reserved[i] = frame32.reserved[i];

	ret = drm_ioctl_kernel(filp, hermes_kms_ioctl_acquire_frame, &frame,
			       DRM_RENDER_ALLOW);
	if (ret)
		return ret;

	/* Actual fields through damage_x2 precede the divergent trailing pad. */
	memcpy(&frame32, &frame,
	       offsetof(struct drm_hermes_kms_acquire_frame_v10_compat,
			damage_x2) + sizeof(frame32.damage_x2));
	memset(frame32.reserved, 0, sizeof(frame32.reserved));
	if (copy_to_user(compat_ptr(arg), &frame32, sizeof(frame32))) {
		hermes_kms_close_frame_fds(&frame);
		return -EFAULT;
	}
	return 0;
}

static long hermes_kms_compat_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg)
{
	if (cmd == DRM_IOCTL_HERMES_KMS_GET_STATUS_V10_COMPAT)
		return hermes_kms_compat_get_status(filp, arg);
	if (cmd == DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME_V10_COMPAT)
		return hermes_kms_compat_acquire_frame(filp, arg);

	return drm_compat_ioctl(filp, cmd, arg);
}
#else
#define hermes_kms_compat_ioctl NULL
#endif

static const struct file_operations hermes_kms_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = hermes_kms_compat_ioctl,
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
	unsigned int i;

	seq_printf(m, "output_count:          %u\n", hdev->output_count);
	for (i = 0; i < hdev->output_count; i++) {
		struct hermes_kms_output *output = &hdev->outputs[i];

		mutex_lock(&output->state_lock);
		seq_printf(m, "\n[%s]\n", output->output_name);
		seq_printf(m, "output_enabled:        %d\n",
			   output->output_enabled);
		seq_printf(m, "owner_pid:             %d\n", output->owner_pid);
		seq_printf(m, "session_id:            %llu\n",
			   output->session_id);
		seq_printf(m, "requested_mode:        %ux%u@%u\n",
			   output->requested_width, output->requested_height,
			   output->requested_refresh_hz);
		seq_printf(m, "vblank_period_ns:      %d\n",
			   READ_ONCE(drm_crtc_vblank_crtc(&output->crtc)->framedur_ns));
		seq_printf(m, "vblank_count:          %llu\n",
			   (unsigned long long)atomic64_read(
				   &output->vblank_count));
		seq_printf(m, "vblank_overrun_count:  %llu\n",
			   (unsigned long long)atomic64_read(
				   &output->vblank_overrun_count));
		seq_printf(m, "frame_sequence:        %llu\n",
			   (unsigned long long)atomic64_read(
				   &output->frame_sequence));
		seq_printf(m, "frame_update_count:    %llu\n",
			   output->frame_update_count);
		seq_printf(m, "cursor_sequence:       %llu\n",
			   (unsigned long long)atomic64_read(
				   &output->cursor_sequence));
		seq_printf(m, "cursor_image_sequence: %llu\n",
			   output->cursor_image_sequence);
		seq_printf(m, "cursor_update_count:   %llu\n",
			   output->cursor_update_count);
		seq_printf(m, "cursor_acquire_count:  %llu\n",
			   output->cursor_acquire_count);
		seq_printf(m, "cursor_wait_count:     %llu\n",
			   output->cursor_wait_count);
		seq_printf(m, "cursor_visible:        %d\n",
			   output->cursor_visible);
		seq_printf(m, "acquire_count:         %llu\n",
			   output->acquire_count);
		seq_printf(m, "acquire_no_frame:      %llu\n",
			   output->acquire_no_frame_count);
		seq_printf(m, "dmabuf_export_count:   %llu\n",
			   output->dmabuf_export_count);
		seq_printf(m, "dmabuf_export_fail:    %llu\n",
			   output->dmabuf_export_fail_count);
		seq_printf(m, "sync_file_export:      %llu\n",
			   output->sync_file_export_count);
		seq_printf(m, "sync_file_export_fail: %llu\n",
			   output->sync_file_export_fail_count);
		seq_printf(m, "wait_count:            %llu\n",
			   output->wait_count);
		seq_printf(m, "wait_timeout_count:    %llu\n",
			   output->wait_timeout_count);
		seq_printf(m, "hotplug_event_count:   %llu\n",
			   output->hotplug_event_count);
		seq_printf(m, "output_enable_count:   %llu\n",
			   output->output_enable_count);
		seq_printf(m, "output_disable_count:  %llu\n",
			   output->output_disable_count);
		mutex_unlock(&output->state_lock);
	}
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

/*
 * Cross-device DMA-BUF importers can impose stricter row-stride requirements
 * than the generic shmem dumb-buffer helper. In particular, radeonsi requires
 * a 256-byte-aligned pitch for linear ARGB/XRGB buffers. Keep the visible
 * width unchanged while padding each backing row so every advertised mode can
 * be imported by the encoding GPU, not only widths that happen to align.
 */
#define HERMES_KMS_PITCH_ALIGN 256

/*
 * Importers also recompute the surface layout from the geometry and refuse a
 * buffer that is smaller than that layout needs, rather than reading past its
 * end. radeonsi does exactly this: si_texture_from_winsys_buffer() rejects the
 * import when surface.total_size exceeds the buffer, and total_size is derived
 * from a *padded* height (ac_surface.c computes
 * surf_slice_size = pitch * surf_height * bpe). A buffer covering only
 * pitch x height therefore imports on hardware that pads by nothing and is
 * rejected on hardware that pads, which is invisible for the common
 * resolutions because their heights are already aligned - 1080, 720 and 1440
 * all are - and shows up on something like 1600x1068.
 *
 * Pad the backing height so the slack is always there. 16 rows covers the
 * alignments importers are known to apply and costs at most 15 rows of
 * padding: 230 KiB on a 4K buffer, nothing on the resolutions that were
 * already aligned. The visible height is untouched; only the allocation grows.
 */
#define HERMES_KMS_IMPORT_HEIGHT_ALIGN 16

static int hermes_kms_dumb_create(struct drm_file *file,
				  struct drm_device *drm,
				  struct drm_mode_create_dumb *args)
{
	struct drm_gem_shmem_object *shmem;
	u64 line_bits;
	u64 pitch;
	u64 padded_height;
	u64 padded_size;
	int ret;

	if (args->flags || !args->width || !args->height || !args->bpp)
		return -EINVAL;
	if (check_mul_overflow((u64)args->width, (u64)args->bpp,
			       &line_bits))
		return -EOVERFLOW;
	pitch = DIV_ROUND_UP_ULL(line_bits, 8);
	if (check_add_overflow(pitch, (u64)HERMES_KMS_PITCH_ALIGN - 1,
			       &pitch))
		return -EOVERFLOW;
	pitch = round_down(pitch, (u64)HERMES_KMS_PITCH_ALIGN);
	if (!pitch || pitch > U32_MAX)
		return -E2BIG;
	args->pitch = pitch;

	padded_height = ALIGN((u64)args->height,
			      HERMES_KMS_IMPORT_HEIGHT_ALIGN);
	if (check_mul_overflow((u64)args->pitch, padded_height,
			       &padded_size))
		return -EOVERFLOW;
	/* size is output-only: never let an untrusted input value choose allocation. */
	if (check_add_overflow(padded_size, (u64)PAGE_SIZE - 1,
			       &padded_size))
		return -EOVERFLOW;
	args->size = round_down(padded_size, (u64)PAGE_SIZE);
	/* drm_gem_shmem_create() takes size_t, while the dumb UAPI uses u64. */
	if (args->size > SIZE_MAX)
		return -E2BIG;

	shmem = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	drm_gem_object_put(&shmem->base);
	return ret;
}

static const struct drm_driver hermes_kms_driver = {
	/*
	 * DRIVER_RENDER exposes a render node (/dev/dri/renderD*). The frame
	 * capture consumer opens that node to call ACQUIRE_FRAME and
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
	.patchlevel = HERMES_KMS_DRIVER_PATCH,
	.fops = &hermes_kms_fops,
	.open = hermes_kms_open,
	.postclose = hermes_kms_postclose,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init = hermes_kms_debugfs_init,
#endif
	.ioctls = hermes_kms_ioctls,
	.num_ioctls = ARRAY_SIZE(hermes_kms_ioctls),
	.gem_prime_import = drm_gem_shmem_prime_import_no_map,
	.dumb_create = hermes_kms_dumb_create,
};

static int hermes_kms_output_modeset_init(struct hermes_kms_output *output)
{
	struct drm_device *drm = &output->hdev->drm;
	int ret;

	ret = drm_connector_init(drm, &output->connector,
				 &hermes_kms_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	drm_connector_helper_add(&output->connector,
				 &hermes_kms_connector_helper_funcs);

	/*
	 * drm_connector_init() attaches the EDID property to every connector
	 * type except VIRTUAL and WRITEBACK, on the assumption that neither has
	 * an EDID to publish. This one does, so attach it explicitly. Without
	 * this, drm_edid_connector_update() fails with -EINVAL because the
	 * property is not on the object, the synthetic EDID never reaches
	 * userspace, and the connector's sysfs "edid" reads back empty — so the
	 * identity and range limits it carries are silently absent, which is
	 * precisely what they exist to provide.
	 */
	drm_connector_attach_edid_property(&output->connector);

	/*
	 * Do not set the connector PATH property here. That property is
	 * reserved for DP-MST tunnelled connectors: drm_connector_set_path_property()
	 * returns -EINVAL on a plain DRM_MODE_CONNECTOR_VIRTUAL connector, and a
	 * PATH blob would mislead userspace (KScreen/KWin) into treating Hermes as
	 * an MST sink. The connector keeps its kernel-assigned "Virtual-N" name,
	 * which is what compositors enumerate. The friendly identity is reported
	 * separately via DRM_IOCTL_HERMES_KMS_GET_IDENTITY (output_name).
	 */

	output->connector.display_info.non_desktop = non_desktop;
	if (drm->mode_config.non_desktop_property)
		drm_object_attach_property(&output->connector.base,
					   drm->mode_config.non_desktop_property,
					   non_desktop ? 1 : 0);
	output->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				   DRM_CONNECTOR_POLL_DISCONNECT;

	/* Primary plane. */
	ret = drm_universal_plane_init(drm, &output->primary, 0,
				       &hermes_kms_plane_funcs,
				       hermes_kms_formats,
				       ARRAY_SIZE(hermes_kms_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&output->primary,
			     &hermes_kms_plane_helper_funcs);

	/*
	 * Advertise FB_DAMAGE_CLIPS so the compositor can tell us which region
	 * changed each frame; we forward it to the capture consumer via
	 * ACQUIRE_FRAME's damage rect so only the dirty region is encoded.
	 */
	drm_plane_enable_fb_damage_clips(&output->primary);

	/* Cursor plane: lets the compositor offload pointer motion (no full
	 * recomposite per move). Consumed client-side, not blended into capture. */
	ret = drm_universal_plane_init(drm, &output->cursor, 0,
				       &hermes_kms_cursor_funcs,
				       hermes_kms_cursor_formats,
				       ARRAY_SIZE(hermes_kms_cursor_formats),
				       NULL, DRM_PLANE_TYPE_CURSOR, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&output->cursor,
			     &hermes_kms_cursor_helper_funcs);

	/* CRTC driven by the software vblank timer. Use the managed variant to
	 * match vkms and pair with devm_drm_dev_alloc(). */
	drm_dbg_kms(drm,
		    "%s primary plane type=%d (PRIMARY=%d) before crtc init\n",
		    output->output_name, output->primary.type,
		    DRM_PLANE_TYPE_PRIMARY);
	ret = drmm_crtc_init_with_planes(drm, &output->crtc,
					 &output->primary, &output->cursor,
					 &hermes_kms_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&output->crtc, &hermes_kms_crtc_helper_funcs);

	/* Encoder linking the CRTC to the connector. */
	ret = drm_encoder_init(drm, &output->encoder,
			       &hermes_kms_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		return ret;
	output->encoder.possible_crtcs = drm_crtc_mask(&output->crtc);

	ret = drm_connector_attach_encoder(&output->connector,
					   &output->encoder);
	if (ret)
		return ret;

	return 0;
}

static int hermes_kms_modeset_init(struct hermes_kms_device *hdev)
{
	struct drm_device *drm = &hdev->drm;
	unsigned int i;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	/*
	 * These are framebuffer limits, not display-mode limits.  Keep them low
	 * enough for cursor buffers; the connector and SET_OUTPUT paths enforce
	 * HERMES_KMS_MIN_WIDTH/HEIGHT for actual modes.
	 * Setting these to the minimum mode size makes the DRM core reject cursor
	 * ADDFB2 requests before fb_create is reached.
	 */
	drm->mode_config.min_width = HERMES_KMS_MIN_FRAMEBUFFER_WIDTH;
	drm->mode_config.min_height = HERMES_KMS_MIN_FRAMEBUFFER_HEIGHT;
	drm->mode_config.max_width = HERMES_KMS_MAX_WIDTH;
	drm->mode_config.max_height = HERMES_KMS_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	/* Standard 256x256 cursor envelope so compositors size HW cursors. */
	drm->mode_config.cursor_width = 256;
	drm->mode_config.cursor_height = 256;
	drm->mode_config.funcs = &hermes_kms_mode_config_funcs;

	for (i = 0; i < hdev->output_count; i++) {
		ret = hermes_kms_output_modeset_init(&hdev->outputs[i]);
		if (ret)
			return ret;
	}

	/* One independently paced software-vblank CRTC per virtual output. */
	ret = drm_vblank_init(drm, hdev->output_count);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

static ssize_t hermes_kms_role_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct hermes_kms_device *hdev = dev_get_drvdata(dev);
	const char *role = "general";

	if (hdev) {
		if (hdev->device_role == HERMES_KMS_DEVICE_ROLE_HOST)
			role = "host";
		else if (hdev->device_role == HERMES_KMS_DEVICE_ROLE_SESSION)
			role = "session";
	}

	return sysfs_emit(buf, "%s\n", role);
}
static DEVICE_ATTR_RO(hermes_kms_role);

static ssize_t hermes_kms_session_index_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct hermes_kms_device *hdev = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", hdev ? hdev->session_index : 0);
}
static DEVICE_ATTR_RO(hermes_kms_session_index);

static int hermes_kms_probe(struct platform_device *pdev)
{
	struct hermes_kms_device *hdev;
	struct drm_device *drm;
	unsigned int output_count = outputs;
	unsigned int i;
	int ret;

	hdev = devm_drm_dev_alloc(&pdev->dev, &hermes_kms_driver,
				  struct hermes_kms_device, drm);
	if (IS_ERR(hdev))
		return PTR_ERR(hdev);

	drm = &hdev->drm;
	platform_set_drvdata(pdev, hdev);
	hdev->device_index = pdev->id >= 0 ? pdev->id : 0;
	hdev->device_count = registered_device_count;
	hdev->session_device_count = session_devices;
	if (session_devices) {
		hdev->device_role = hdev->device_index == 0 ?
				      HERMES_KMS_DEVICE_ROLE_HOST :
				      HERMES_KMS_DEVICE_ROLE_SESSION;
		hdev->session_index = hdev->device_index;
	} else if (registered_device_count > 1) {
		/* Preserve the UAPI v9 devices=N all-private layout. */
		hdev->device_role = HERMES_KMS_DEVICE_ROLE_SESSION;
		hdev->session_index = hdev->device_index + 1;
		hdev->session_device_count = registered_device_count;
	} else {
		hdev->device_role = HERMES_KMS_DEVICE_ROLE_GENERAL;
		hdev->session_index = 0;
	}
	if (output_count < 1 || output_count > HERMES_KMS_MAX_OUTPUTS) {
		output_count = clamp(output_count, 1u,
				     (unsigned int)HERMES_KMS_MAX_OUTPUTS);
		drm_warn(drm, "outputs=%u out of range, using %u\n",
			 outputs, output_count);
	}
	hdev->output_count = output_count;

	for (i = 0; i < hdev->output_count; i++) {
		struct hermes_kms_output *output = &hdev->outputs[i];
		u8 checksum = 0;
		unsigned int j;

		output->hdev = hdev;
		output->index = i;
		snprintf(output->output_name, sizeof(output->output_name),
			 HERMES_KMS_OUTPUT_NAME_PREFIX "%u",
			 hdev->device_index * hdev->output_count + i + 1);

		/*
		 * Give every connector a stable, distinct EDID serial. KWin uses
		 * EDID identity when persisting layouts; identical virtual panels
		 * would otherwise be easy to collapse or swap across restarts.
		 */
		memcpy(output->edid, hermes_kms_edid, sizeof(output->edid));
		j = hdev->device_index * hdev->output_count + i + 1;
		output->edid[12] = j & 0xff;
		output->edid[13] = (j >> 8) & 0xff;
		output->edid[14] = (j >> 16) & 0xff;
		output->edid[15] = (j >> 24) & 0xff;
		j = 0;
		for (j = 0; j < sizeof(output->edid) - 1; j++)
			checksum += output->edid[j];
		output->edid[sizeof(output->edid) - 1] = -checksum;

		mutex_init(&output->state_lock);
		mutex_init(&output->export_lock);
		init_waitqueue_head(&output->frame_wait);
		atomic64_set(&output->authorization_generation, 1);
		atomic64_set(&output->frame_sequence, 0);
		atomic64_set(&output->cursor_sequence, 0);
		atomic64_set(&output->vblank_count, 0);
		atomic64_set(&output->vblank_overrun_count, 0);
		ratelimit_state_init(&output->output_change_ratelimit,
				     HERMES_KMS_OUTPUT_CHANGE_INTERVAL,
				     HERMES_KMS_OUTPUT_CHANGE_BURST);
		output->next_session_id = 1;
		hermes_kms_init_output_state(drm, output);
	}

	/*
	 * Publish parent attributes before drm_dev_register() emits the DRM card
	 * uevent. The packaged rule uses them to keep the host card on seat0 and
	 * map only private session cards to dedicated seats.
	 */
	ret = device_create_file(&pdev->dev, &dev_attr_hermes_kms_role);
	if (ret)
		return ret;
	ret = device_create_file(&pdev->dev,
				 &dev_attr_hermes_kms_session_index);
	if (ret)
		goto err_remove_role;

	ret = hermes_kms_modeset_init(hdev);
	if (ret)
		goto err_remove_session_index;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_remove_session_index;

	drm_info(drm,
		 "registered Hermes-KMS virtual DRM device index=%u/%u role=%u session_index=%u/%u outputs=%u initial_enabled=%d hotplug_events=%d non_desktop=%d initial_mode=%ux%u@%u\n",
		 hdev->device_index + 1,
		 hdev->device_count,
		 hdev->device_role,
		 hdev->session_index,
		 hdev->session_device_count,
		 hdev->output_count,
		 initial_enabled,
		 hotplug_events,
		 non_desktop,
		 hdev->outputs[0].requested_width,
		 hdev->outputs[0].requested_height,
		 hdev->outputs[0].requested_refresh_hz);

	/*
	 * initial_enabled exists for driver development against modetest and
	 * friends. Outside that, it hands a connected connector to whatever
	 * compositor is running before Hermes can own it: the desktop extends
	 * onto a virtual output nobody streams to and the user sees a black
	 * screen. It usually arrives from a stale
	 * /etc/modprobe.d/hermes-kms.conf, which overrides the disconnected
	 * default this driver ships, so say where to look.
	 */
	if (initial_enabled)
		drm_warn(drm,
			 "initial_enabled=1 connects a virtual output with no owner; a compositor may extend the desktop onto it. Check /etc/modprobe.d for an override of the packaged initial_enabled=0 default.\n");

	return 0;

err_remove_session_index:
	device_remove_file(&pdev->dev,
			   &dev_attr_hermes_kms_session_index);
err_remove_role:
	device_remove_file(&pdev->dev, &dev_attr_hermes_kms_role);
	return ret;
}

static void hermes_kms_remove(struct platform_device *pdev)
{
	struct hermes_kms_device *hdev = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < hdev->output_count; i++) {
		struct hermes_kms_output *output = &hdev->outputs[i];

		mutex_lock(&output->state_lock);
		if (output->output_enabled) {
			output->output_enabled = false;
			output->last_disable_ns = ktime_get_ns();
			output->output_disable_count++;
		}
		hermes_kms_clear_owner_locked(output);
		mutex_unlock(&output->state_lock);
		hermes_kms_track_frame(output, NULL, NULL, false);
		hermes_kms_track_cursor(output, NULL, false);
		wake_up_interruptible(&output->frame_wait);
	}

	drm_dev_unregister(&hdev->drm);
	drm_atomic_helper_shutdown(&hdev->drm);

	for (i = 0; i < hdev->output_count; i++) {
		struct hermes_kms_output *output = &hdev->outputs[i];
		struct drm_framebuffer *fb;
		struct drm_framebuffer *cursor_fb;

		mutex_lock(&output->state_lock);
		fb = output->framebuffer;
		cursor_fb = output->cursor_framebuffer;
		output->framebuffer = NULL;
		output->cursor_framebuffer = NULL;
		hermes_kms_clear_frame_locked(output);
		hermes_kms_clear_cursor_metadata_locked(output);
		mutex_unlock(&output->state_lock);

		mutex_lock(&output->export_lock);
		hermes_kms_drop_export_cache_locked(&output->frame_export_cache);
		hermes_kms_drop_export_cache_locked(&output->cursor_export_cache);
		mutex_unlock(&output->export_lock);

		if (fb)
			drm_framebuffer_put(fb);
		if (cursor_fb)
			drm_framebuffer_put(cursor_fb);
	}

	device_remove_file(&pdev->dev, &dev_attr_hermes_kms_session_index);
	device_remove_file(&pdev->dev, &dev_attr_hermes_kms_role);
}

static struct platform_driver hermes_kms_platform_driver = {
	.probe = hermes_kms_probe,
	.remove = hermes_kms_remove,
	.driver = {
		.name = HERMES_KMS_DRIVER_NAME,
	},
};

static struct platform_device *
hermes_kms_platform_devices[HERMES_KMS_MAX_REGISTERED_DEVICES];

static int __init hermes_kms_init(void)
{
	unsigned int device_count = devices;
	unsigned int i;
	int ret;

	if (session_devices > HERMES_KMS_MAX_SESSION_DEVICES) {
		pr_warn("%s: session_devices=%u out of range, using %u\n",
			HERMES_KMS_DRIVER_NAME, session_devices,
			HERMES_KMS_MAX_SESSION_DEVICES);
		session_devices = HERMES_KMS_MAX_SESSION_DEVICES;
	}

	if (session_devices) {
		if (devices != HERMES_KMS_DEFAULT_DEVICES)
			pr_warn("%s: session_devices=%u overrides devices=%u\n",
				HERMES_KMS_DRIVER_NAME, session_devices, devices);
		device_count = 1 + session_devices;
	} else if (device_count < 1 || device_count > HERMES_KMS_MAX_DEVICES) {
		device_count = clamp(device_count, 1u,
				     (unsigned int)HERMES_KMS_MAX_DEVICES);
		pr_warn("%s: devices=%u out of range, using %u\n",
			HERMES_KMS_DRIVER_NAME, devices, device_count);
		devices = device_count;
	}
	registered_device_count = device_count;

	ret = platform_driver_register(&hermes_kms_platform_driver);
	if (ret)
		return ret;

	for (i = 0; i < device_count; i++) {
		struct platform_device *pdev;

		/*
		 * Preserve the original "hermes-kms" platform path for devices=1.
		 * Multi-device mode deliberately uses hermes-kms.0..N so udev can
		 * assign every independent DRM-master domain to a stable seat.
		 */
		pdev = platform_device_alloc(HERMES_KMS_DRIVER_NAME,
					     device_count == 1 ?
					     PLATFORM_DEVID_NONE : (int)i);
		if (!pdev) {
			ret = -ENOMEM;
			goto err_unregister_devices;
		}
		ret = platform_device_add(pdev);
		if (ret) {
			platform_device_put(pdev);
			goto err_unregister_devices;
		}
		hermes_kms_platform_devices[i] = pdev;
	}

	pr_info("%s: module loaded devices=%u session_devices=%u outputs_per_device=%u\n",
		HERMES_KMS_DRIVER_NAME, device_count, session_devices, outputs);
	return 0;

err_unregister_devices:
	while (i > 0) {
		i--;
		platform_device_unregister(hermes_kms_platform_devices[i]);
		hermes_kms_platform_devices[i] = NULL;
	}
	platform_driver_unregister(&hermes_kms_platform_driver);
	return ret;
}

static void __exit hermes_kms_exit(void)
{
	unsigned int i;

	for (i = registered_device_count; i > 0; i--) {
		if (hermes_kms_platform_devices[i - 1]) {
			platform_device_unregister(
				hermes_kms_platform_devices[i - 1]);
			hermes_kms_platform_devices[i - 1] = NULL;
		}
	}
	platform_driver_unregister(&hermes_kms_platform_driver);
	pr_info("%s: module unloaded\n", HERMES_KMS_DRIVER_NAME);
}

module_init(hermes_kms_init);
module_exit(hermes_kms_exit);

MODULE_AUTHOR("Hermes contributors");
MODULE_DESCRIPTION(HERMES_KMS_DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
