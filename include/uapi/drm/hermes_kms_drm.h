/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Userspace API for Hermes-KMS.
 *
 * This header is consumed by Hermes and debugging tools. Keep all structs
 * fixed-size and append-only.
 */

#ifndef HERMES_KMS_DRM_H
#define HERMES_KMS_DRM_H

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define HERMES_KMS_UAPI_VERSION 2

#define HERMES_KMS_NAME_LEN 32

#define HERMES_KMS_CAP_VIRTUAL_OUTPUT		(1ULL << 0)
#define HERMES_KMS_CAP_OUTPUT_CONTROL		(1ULL << 1)
#define HERMES_KMS_CAP_DUMB_BUFFERS		(1ULL << 2)
#define HERMES_KMS_CAP_PRIME_IMPORT		(1ULL << 3)
#define HERMES_KMS_CAP_FRAME_METADATA		(1ULL << 4)
#define HERMES_KMS_CAP_FRAME_ACQUIRE		(1ULL << 5)
#define HERMES_KMS_CAP_DMABUF_EXPORT_PLANNED	(1ULL << 32)
#define HERMES_KMS_CAP_ZERO_COPY_TARGET		(1ULL << 33)

#define HERMES_KMS_STATUS_OUTPUT_ENABLED	(1ULL << 0)
#define HERMES_KMS_STATUS_CONNECTED		(1ULL << 1)
#define HERMES_KMS_STATUS_SCANOUT_ACTIVE	(1ULL << 2)
#define HERMES_KMS_STATUS_FRAME_VALID		(1ULL << 3)
#define HERMES_KMS_STATUS_DMABUF_EXPORT_READY	(1ULL << 4)

#define HERMES_KMS_FRAME_REQUEST_DMABUF		(1ULL << 0)
#define HERMES_KMS_FRAME_METADATA_VALID		(1ULL << 1)
#define HERMES_KMS_FRAME_DMABUF_VALID		(1ULL << 2)
#define HERMES_KMS_FRAME_SYNC_FILE_VALID	(1ULL << 3)
#define HERMES_KMS_FRAME_COPY_FALLBACK_REQUIRED	(1ULL << 4)

struct drm_hermes_kms_version {
	__u32 uapi_version;
	__u32 driver_major;
	__u32 driver_minor;
	__u32 driver_patch;
	char driver_name[HERMES_KMS_NAME_LEN];
};

struct drm_hermes_kms_caps {
	__u64 flags;
	__u32 min_width;
	__u32 min_height;
	__u32 max_width;
	__u32 max_height;
	__u32 preferred_width;
	__u32 preferred_height;
	__u32 max_refresh_hz;
	__u32 reserved0;
};

struct drm_hermes_kms_status {
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
	__u64 reserved[8];
};

struct drm_hermes_kms_set_output {
	__u32 enabled;
	__u32 width;
	__u32 height;
	__u32 refresh_hz;
	__u32 reserved[4];
};

struct drm_hermes_kms_acquire_frame {
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
	__u64 reserved[8];
};

#define DRM_HERMES_KMS_GET_VERSION	0x00
#define DRM_HERMES_KMS_GET_CAPS		0x01
#define DRM_HERMES_KMS_GET_STATUS	0x02
#define DRM_HERMES_KMS_SET_OUTPUT	0x03
#define DRM_HERMES_KMS_ACQUIRE_FRAME	0x04

#define DRM_IOCTL_HERMES_KMS_GET_VERSION \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_VERSION, struct drm_hermes_kms_version)
#define DRM_IOCTL_HERMES_KMS_GET_CAPS \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_CAPS, struct drm_hermes_kms_caps)
#define DRM_IOCTL_HERMES_KMS_GET_STATUS \
	DRM_IOR(DRM_COMMAND_BASE + DRM_HERMES_KMS_GET_STATUS, struct drm_hermes_kms_status)
#define DRM_IOCTL_HERMES_KMS_SET_OUTPUT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_HERMES_KMS_SET_OUTPUT, struct drm_hermes_kms_set_output)
#define DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_HERMES_KMS_ACQUIRE_FRAME, struct drm_hermes_kms_acquire_frame)

#if defined(__cplusplus)
}
#endif

#endif /* HERMES_KMS_DRM_H */
