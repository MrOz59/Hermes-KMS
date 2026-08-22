/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace API for Hermes-KMS.
 *
 * This header is consumed by capture clients and debugging tools. Keep structs
 * fixed-size and append-only.
 */

#ifndef HERMES_KMS_DRM_H
#define HERMES_KMS_DRM_H

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define HERMES_KMS_UAPI_VERSION 11

#define HERMES_KMS_NAME_LEN 32

#define HERMES_KMS_CAP_VIRTUAL_OUTPUT		(1ULL << 0)
#define HERMES_KMS_CAP_OUTPUT_CONTROL		(1ULL << 1)
#define HERMES_KMS_CAP_DUMB_BUFFERS		(1ULL << 2)
#define HERMES_KMS_CAP_PRIME_IMPORT		(1ULL << 3)
#define HERMES_KMS_CAP_FRAME_METADATA		(1ULL << 4)
#define HERMES_KMS_CAP_FRAME_ACQUIRE		(1ULL << 5)
#define HERMES_KMS_CAP_DMABUF_EXPORT		(1ULL << 6)
#define HERMES_KMS_CAP_OUTPUT_IDENTITY		(1ULL << 7)
#define HERMES_KMS_CAP_SESSION_OWNER		(1ULL << 8)
#define HERMES_KMS_CAP_FRAME_WAIT		(1ULL << 9)
#define HERMES_KMS_CAP_METRICS			(1ULL << 10)
#define HERMES_KMS_CAP_MULTI_OUTPUT		(1ULL << 11)
#define HERMES_KMS_CAP_MULTI_DEVICE		(1ULL << 12)
#define HERMES_KMS_CAP_SESSION_DEVICE_POOL	(1ULL << 13)
#define HERMES_KMS_CAP_SESSION_TOKEN		(1ULL << 14)
#define HERMES_KMS_CAP_CURSOR_CAPTURE		(1ULL << 15)
#define HERMES_KMS_CAP_DMABUF_EXPORT_PLANNED	(1ULL << 32)
#define HERMES_KMS_CAP_ZERO_COPY_TARGET		(1ULL << 33)
#define HERMES_KMS_CAP_WRITEBACK_CONNECTOR	(1ULL << 34)
#define HERMES_KMS_CAP_SYNC_FILE		(1ULL << 35)

#define HERMES_KMS_STATUS_OUTPUT_ENABLED	(1ULL << 0)
#define HERMES_KMS_STATUS_CONNECTED		(1ULL << 1)
#define HERMES_KMS_STATUS_SCANOUT_ACTIVE	(1ULL << 2)
#define HERMES_KMS_STATUS_FRAME_VALID		(1ULL << 3)
#define HERMES_KMS_STATUS_DMABUF_EXPORT_READY	(1ULL << 4)
#define HERMES_KMS_STATUS_SESSION_OWNED		(1ULL << 5)
#define HERMES_KMS_STATUS_HOTPLUG_EVENTS_ENABLED (1ULL << 6)

#define HERMES_KMS_FRAME_REQUEST_DMABUF		(1ULL << 0)
#define HERMES_KMS_FRAME_METADATA_VALID		(1ULL << 1)
#define HERMES_KMS_FRAME_DMABUF_VALID		(1ULL << 2)
#define HERMES_KMS_FRAME_SYNC_FILE_VALID	(1ULL << 3)
#define HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED	(1ULL << 4)
#define HERMES_KMS_FRAME_REQUEST_SYNC_FILE	(1ULL << 5)
/*
 * Set when damage_x1/y1/x2/y2 describe the region changed since the previous
 * frame (from the compositor's FB_DAMAGE_CLIPS). When DAMAGE_VALID is clear the
 * consumer must treat the whole frame as dirty. This includes the first
 * ACQUIRE_FRAME on an fd and any acquire that skipped one or more sequences;
 * per-frame damage cannot safely describe such a gap. Damage is a half-open
 * rect: [x1,x2) x [y1,y2); an empty rect means "no change".
 */
#define HERMES_KMS_FRAME_DAMAGE_VALID		(1ULL << 6)

#define HERMES_KMS_WAIT_FRAME_READY		(1ULL << 0)

#define HERMES_KMS_WAIT_UPDATE_FRAME_READY	(1ULL << 0)
#define HERMES_KMS_WAIT_UPDATE_CURSOR_READY	(1ULL << 1)

#define HERMES_KMS_CURSOR_REQUEST_DMABUF	(1ULL << 0)
#define HERMES_KMS_CURSOR_REQUEST_SYNC_FILE	(1ULL << 1)
#define HERMES_KMS_CURSOR_METADATA_VALID	(1ULL << 2)
#define HERMES_KMS_CURSOR_VISIBLE		(1ULL << 3)
#define HERMES_KMS_CURSOR_POSITION_VALID	(1ULL << 4)
#define HERMES_KMS_CURSOR_HOTSPOT_VALID		(1ULL << 5)
#define HERMES_KMS_CURSOR_BUFFER_VALID		(1ULL << 6)
#define HERMES_KMS_CURSOR_DMABUF_VALID		(1ULL << 7)
#define HERMES_KMS_CURSOR_SYNC_FILE_VALID	(1ULL << 8)
#define HERMES_KMS_CURSOR_GEOMETRY_VALID	(1ULL << 9)

#define HERMES_KMS_SET_OUTPUT_RESULT_CONNECTED		(1U << 0)
#define HERMES_KMS_SET_OUTPUT_RESULT_OWNER_ASSIGNED	(1U << 1)
#define HERMES_KMS_SET_OUTPUT_RESULT_HOTPLUG_SENT	(1U << 2)

/*
 * Device roles reported by GET_IDENTITY (uapi >= 10).
 *
 * GENERAL preserves the v9 devices=N layout. HOST is the seat0-compatible
 * card in the packaged session pool. SESSION cards are reserved for private
 * compositors and carry a stable 1-based session_index.
 */
#define HERMES_KMS_DEVICE_ROLE_GENERAL	0U
#define HERMES_KMS_DEVICE_ROLE_HOST	1U
#define HERMES_KMS_DEVICE_ROLE_SESSION	2U

struct drm_hermes_kms_version {
	__u32 uapi_version;
	__u32 driver_major;
	__u32 driver_minor;
	__u32 driver_patch;
	char driver_name[HERMES_KMS_NAME_LEN];
};

struct drm_hermes_kms_caps {
	__aligned_u64 flags;
	__u32 min_width;
	__u32 min_height;
	__u32 max_width;
	__u32 max_height;
	__u32 preferred_width;
	__u32 preferred_height;
	__u32 max_refresh_hz;
	/* Number of independently selectable outputs on this DRM device. */
	__u32 output_count;
};

struct drm_hermes_kms_status {
	__aligned_u64 flags;
	__aligned_u64 frame_sequence;
	__aligned_u64 last_update_ns;
	__aligned_u64 last_enable_ns;
	__aligned_u64 last_disable_ns;
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
	/* Explicitly replace LP64's implicit pad so ILP32 has the same ABI. */
	__u32 reserved_alignment;
	__aligned_u64 framebuffer_modifier;
	__aligned_u64 session_id;
	/* Diagnostic only; authorization is fd/token based, never PID based. */
	__s32 owner_pid;
	__u32 reserved0;
	__aligned_u64 reserved[6];
};

struct drm_hermes_kms_identity {
	char driver_name[HERMES_KMS_NAME_LEN];
	char output_name[HERMES_KMS_NAME_LEN];
	char connector_name[HERMES_KMS_NAME_LEN];
	__u32 connector_id;
	__u32 crtc_id;
	__u32 plane_id;
	__u32 encoder_id;
	/* 0-based selected output and total outputs (uapi >= 8). */
	__u32 output_index;
	__u32 output_count;
	/*
	 * 0-based DRM device index and total devices created by this module
	 * (uapi >= 9). Each device has an independent DRM-master domain, which
	 * lets a separate compositor own each streaming session.
	 */
	__u32 device_index;
	__u32 device_count;
	/* Role and stable private-seat identity (uapi >= 10). */
	__u32 device_role;
	__u32 session_index;
	__u32 session_device_count;
	/* Cursor plane object used by the separate cursor-capture stream. */
	__u32 cursor_plane_id;
};

/*
 * Bind this DRM file descriptor to one virtual output. All output-scoped
 * ioctls on the descriptor (status, control, capture, wait and metrics) then
 * operate on that output. A newly opened descriptor is bound to output 0 for
 * compatibility with uapi <= 7 clients.
 *
 * Rebinding an fd that currently owns an enabled output is rejected with
 * -EBUSY. Use a separate fd for each concurrent output/session.
 */
struct drm_hermes_kms_select_output {
	__u32 output_index;
	__u32 flags;
	__u32 selected_output_index;
	__u32 output_count;
	char output_name[HERMES_KMS_NAME_LEN];
	__u32 reserved[8];
};

struct drm_hermes_kms_set_output {
	__u32 enabled;
	__u32 width;
	__u32 height;
	__u32 refresh_hz;
	__u32 flags;
	__u32 result_flags;
	__aligned_u64 session_id;
};

struct drm_hermes_kms_acquire_frame {
	__aligned_u64 flags;
	__aligned_u64 sequence;
	__aligned_u64 timestamp_ns;
	__aligned_u64 modifier;
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
	/*
	 * Damage rectangle for this frame, valid only when
	 * HERMES_KMS_FRAME_DAMAGE_VALID is set in flags (half-open:
	 * [damage_x1, damage_x2) x [damage_y1, damage_y2), in pixels).
	 */
	__u32 damage_x1;
	__u32 damage_y1;
	__u32 damage_x2;
	__u32 damage_y2;
	__aligned_u64 reserved[6];
};

struct drm_hermes_kms_wait_frame {
	__aligned_u64 flags;
	__aligned_u64 after_sequence;
	__aligned_u64 sequence;
	__aligned_u64 timestamp_ns;
	__aligned_u64 status_flags;
	__u32 timeout_ms;
	__u32 reserved0;
	__aligned_u64 reserved[6];
};

/*
 * Wait until either the primary-frame or separately tracked cursor stream
 * advances. Cursor-only commits deliberately do not advance WAIT_FRAME. The
 * request supplies both last-consumed sequences and timeout_ms, with every
 * other field zero. A successful response sets one or both READY flags and
 * returns coherent current sequences, timestamps and output status. Session
 * revocation wakes the wait and fails it with EACCES.
 */
struct drm_hermes_kms_wait_update {
	__aligned_u64 flags;
	__aligned_u64 after_frame_sequence;
	__aligned_u64 after_cursor_sequence;
	__aligned_u64 frame_sequence;
	__aligned_u64 cursor_sequence;
	__aligned_u64 frame_timestamp_ns;
	__aligned_u64 cursor_timestamp_ns;
	__aligned_u64 status_flags;
	__u32 timeout_ms;
	__u32 reserved0;
	__aligned_u64 reserved[5];
};

/*
 * Latest separately scanned-out cursor state. position_x/y are the raw cursor
 * position supplied in CRTC coordinates. crtc_x/y/w/h are the clipped
 * destination rectangle in integer pixels; src_x/y/w/h are the corresponding
 * clipped source rectangle in DRM 16.16 fixed-point pixels. When supplied,
 * the hotspot is relative to the full, uncropped cursor image. Universal KMS
 * cursor planes normally leave HOTSPOT_VALID clear because the compositor has
 * already accounted for its logical hotspot in position_x/y. POSITION_VALID,
 * GEOMETRY_VALID and HOTSPOT_VALID state which groups are meaningful. VISIBLE
 * is independent of BUFFER_VALID so a fully clipped cursor can retain a
 * reusable image. image_sequence advances whenever pixels may need refreshing
 * (conservatively, every committed state carrying a cursor buffer); sequence
 * advances for every cursor state update, including movement and visibility.
 *
 * Metadata-only acquire is the default. REQUEST_DMABUF and REQUEST_SYNC_FILE
 * request ordinary Linux fds for the current cursor buffer and its write
 * fence. As with primary-frame capture, revocation prevents new exports but
 * cannot recall a DMA-BUF fd that was already returned.
 */
struct drm_hermes_kms_acquire_cursor {
	__aligned_u64 flags;
	__aligned_u64 sequence;
	__aligned_u64 image_sequence;
	__aligned_u64 timestamp_ns;
	__aligned_u64 modifier;
	__aligned_u64 session_id;
	__s32 position_x;
	__s32 position_y;
	__s32 crtc_x;
	__s32 crtc_y;
	__u32 crtc_w;
	__u32 crtc_h;
	__u32 src_x;
	__u32 src_y;
	__u32 src_w;
	__u32 src_h;
	__s32 hotspot_x;
	__s32 hotspot_y;
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
	/* Explicitly preserve identical LP64/ILP32 trailing alignment. */
	__u32 reserved_alignment;
	__aligned_u64 reserved[6];
};

struct drm_hermes_kms_metrics {
	__aligned_u64 frame_sequence;
	__aligned_u64 frame_update_count;
	__aligned_u64 acquire_count;
	__aligned_u64 acquire_no_frame_count;
	__aligned_u64 dmabuf_export_count;
	__aligned_u64 dmabuf_export_fail_count;
	__aligned_u64 sync_file_export_count;
	__aligned_u64 sync_file_export_fail_count;
	__aligned_u64 wait_count;
	__aligned_u64 wait_ready_count;
	__aligned_u64 wait_timeout_count;
	__aligned_u64 wait_interrupted_count;
	__aligned_u64 output_enable_count;
	__aligned_u64 output_disable_count;
	__aligned_u64 hotplug_event_count;
	__aligned_u64 owner_close_disconnect_count;
	__aligned_u64 last_update_ns;
	__aligned_u64 last_acquire_ns;
	__aligned_u64 last_wait_start_ns;
	__aligned_u64 last_wait_end_ns;
	__aligned_u64 last_wait_duration_ns;
	__aligned_u64 last_dmabuf_export_ns;
	__aligned_u64 last_sync_file_export_ns;
	/*
	 * Software-vblank timer callbacks and estimated timer periods missed
	 * between callbacks. Added in UAPI 11 by consuming two reserved slots;
	 * the structure size is unchanged.
	 */
	__aligned_u64 vblank_count;
	__aligned_u64 vblank_overrun_count;
	__aligned_u64 reserved[14];
};

/*
 * Generic session capability handoff (UAPI >= 11).
 *
 * GET_TOKEN input is operation=GET_TOKEN with every other field zero; it
 * operates on the fd's selected output and returns token, session_id,
 * output_index and RESULT_TOKEN_VALID. The caller must own that output.
 *
 * BIND input is operation=BIND plus token, session_id and output_index, with
 * flags/reserved zero. On success the fd is atomically selected and authorized
 * for that exact output/session; the response returns operation, session_id,
 * output_index and RESULT_BOUND (the token fields are cleared). UNBIND only
 * requires operation=UNBIND with all other input fields zero and clears the
 * fd's capture authorization.
 *
 * The output owner passes the opaque token over an application-defined trusted
 * channel. Tokens are invalidated when the owner disables the output or closes
 * its fd. Revocation prevents later protected ioctls and later fd exports; it
 * cannot recall a DMA-BUF fd already returned to userspace. A process holding
 * such an fd retains that buffer capability until it closes the fd, so
 * untrusted consumers require a broker/copy boundary or per-session BO
 * isolation. Tokens are intentionally not tied to a process name, UID or TGID,
 * so brokers and sandboxed capture processes can use the interface without
 * driver-specific policy.
 */
#define HERMES_KMS_SESSION_ACCESS_GET_TOKEN	1U
#define HERMES_KMS_SESSION_ACCESS_BIND		2U
#define HERMES_KMS_SESSION_ACCESS_UNBIND	3U

#define HERMES_KMS_SESSION_ACCESS_RESULT_BOUND	(1U << 0)
#define HERMES_KMS_SESSION_ACCESS_RESULT_TOKEN_VALID (1U << 1)

struct drm_hermes_kms_session_access {
	__aligned_u64 token[2];
	__aligned_u64 session_id;
	__u32 operation;
	__u32 output_index;
	__u32 flags;
	__u32 result_flags;
	__aligned_u64 reserved[4];
};

#define DRM_HERMES_KMS_GET_VERSION	0x00
#define DRM_HERMES_KMS_GET_CAPS		0x01
#define DRM_HERMES_KMS_GET_STATUS	0x02
#define DRM_HERMES_KMS_SET_OUTPUT	0x03
#define DRM_HERMES_KMS_ACQUIRE_FRAME	0x04
#define DRM_HERMES_KMS_GET_IDENTITY	0x05
#define DRM_HERMES_KMS_WAIT_FRAME	0x06
#define DRM_HERMES_KMS_GET_METRICS	0x07
#define DRM_HERMES_KMS_SELECT_OUTPUT	0x08
#define DRM_HERMES_KMS_SESSION_ACCESS	0x09
#define DRM_HERMES_KMS_ACQUIRE_CURSOR	0x0a
#define DRM_HERMES_KMS_WAIT_UPDATE	0x0b

#define DRM_IOCTL_HERMES_KMS_GET_VERSION \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_VERSION, struct drm_hermes_kms_version)
#define DRM_IOCTL_HERMES_KMS_GET_CAPS \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_CAPS, struct drm_hermes_kms_caps)
#define DRM_IOCTL_HERMES_KMS_GET_STATUS \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_STATUS, struct drm_hermes_kms_status)
#define DRM_IOCTL_HERMES_KMS_SET_OUTPUT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_SET_OUTPUT, struct drm_hermes_kms_set_output)
#define DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_ACQUIRE_FRAME, struct drm_hermes_kms_acquire_frame)
#define DRM_IOCTL_HERMES_KMS_GET_IDENTITY \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_IDENTITY, struct drm_hermes_kms_identity)
#define DRM_IOCTL_HERMES_KMS_WAIT_FRAME \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_WAIT_FRAME, struct drm_hermes_kms_wait_frame)
#define DRM_IOCTL_HERMES_KMS_GET_METRICS \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_METRICS, struct drm_hermes_kms_metrics)
#define DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_SELECT_OUTPUT, \
		 struct drm_hermes_kms_select_output)
#define DRM_IOCTL_HERMES_KMS_SESSION_ACCESS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_SESSION_ACCESS, \
		 struct drm_hermes_kms_session_access)
#define DRM_IOCTL_HERMES_KMS_ACQUIRE_CURSOR \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_ACQUIRE_CURSOR, \
		 struct drm_hermes_kms_acquire_cursor)
#define DRM_IOCTL_HERMES_KMS_WAIT_UPDATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_WAIT_UPDATE, \
		 struct drm_hermes_kms_wait_update)

#if defined(__cplusplus)
}
#endif

#endif /* HERMES_KMS_DRM_H */
