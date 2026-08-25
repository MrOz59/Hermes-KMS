/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compile-time regression test for the public Hermes-KMS ioctl ABI.
 *
 * Compile this translation unit for every supported userspace ABI.  In
 * particular, an x86 build with -m32 catches implicit padding and alignment
 * differences that would otherwise give compat tasks different ioctl numbers.
 */

#include <stddef.h>

#include <drm/hermes_kms_drm.h>

#define ASSERT_SIZE(type, expected) \
	_Static_assert(sizeof(type) == (expected), "unexpected size: " #type)
#define ASSERT_OFFSET(type, member, expected) \
	_Static_assert(offsetof(type, member) == (expected), \
		       "unexpected offset: " #type "." #member)
#define ASSERT_IOCTL(name, command, direction, size, value) \
	_Static_assert(_IOC_NR(name) == DRM_COMMAND_BASE + (command), \
		       "unexpected ioctl nr: " #name); \
	_Static_assert(_IOC_TYPE(name) == DRM_IOCTL_BASE, \
		       "unexpected ioctl type: " #name); \
	_Static_assert(_IOC_DIR(name) == (direction), \
		       "unexpected ioctl direction: " #name); \
	_Static_assert(_IOC_SIZE(name) == (size), \
		       "unexpected ioctl payload size: " #name); \
	_Static_assert((unsigned long)(name) == (value), \
		       "unexpected encoded ioctl value: " #name)

_Static_assert(HERMES_KMS_UAPI_VERSION == 13,
	       "update the ABI test when intentionally changing the UAPI");

/*
 * __aligned_u64 is a declaration macro, not a portable standalone type name.
 * Check it in the member position for which linux/types.h defines it; using it
 * directly in _Alignof makes Clang correctly warn that the attribute is being
 * ignored while parsing a type.
 */
struct hermes_aligned_u64_probe {
	unsigned char prefix;
	__aligned_u64 value;
};

_Static_assert(sizeof(((struct hermes_aligned_u64_probe *)0)->value) == 8,
	       "__aligned_u64 must be 64-bit");
_Static_assert(offsetof(struct hermes_aligned_u64_probe, value) == 8,
	       "__aligned_u64 must retain 64-bit alignment");
_Static_assert(sizeof(struct hermes_aligned_u64_probe) == 16,
	       "__aligned_u64 must retain tail padding");

_Static_assert(DRM_HERMES_KMS_GET_VERSION == 0x00, "GET_VERSION command moved");
_Static_assert(DRM_HERMES_KMS_GET_CAPS == 0x01, "GET_CAPS command moved");
_Static_assert(DRM_HERMES_KMS_GET_STATUS == 0x02, "GET_STATUS command moved");
_Static_assert(DRM_HERMES_KMS_SET_OUTPUT == 0x03, "SET_OUTPUT command moved");
_Static_assert(DRM_HERMES_KMS_ACQUIRE_FRAME == 0x04,
	       "ACQUIRE_FRAME command moved");
_Static_assert(DRM_HERMES_KMS_GET_IDENTITY == 0x05,
	       "GET_IDENTITY command moved");
_Static_assert(DRM_HERMES_KMS_WAIT_FRAME == 0x06, "WAIT_FRAME command moved");
_Static_assert(DRM_HERMES_KMS_GET_METRICS == 0x07,
	       "GET_METRICS command moved");
_Static_assert(DRM_HERMES_KMS_SELECT_OUTPUT == 0x08,
	       "SELECT_OUTPUT command moved");
_Static_assert(DRM_HERMES_KMS_SESSION_ACCESS == 0x09,
	       "SESSION_ACCESS command moved");
_Static_assert(DRM_HERMES_KMS_ACQUIRE_CURSOR == 0x0a,
	       "ACQUIRE_CURSOR command moved");
_Static_assert(DRM_HERMES_KMS_WAIT_UPDATE == 0x0b,
	       "WAIT_UPDATE command moved");
_Static_assert(HERMES_KMS_CAP_SESSION_TOKEN == (1ULL << 14),
	       "session-token capability bit moved");
_Static_assert(HERMES_KMS_CAP_CURSOR_CAPTURE == (1ULL << 15),
	       "cursor-capture capability bit moved");
_Static_assert(HERMES_KMS_SESSION_ACCESS_GET_TOKEN == 1,
	       "GET_TOKEN operation moved");
_Static_assert(HERMES_KMS_SESSION_ACCESS_BIND == 2,
	       "BIND operation moved");
_Static_assert(HERMES_KMS_SESSION_ACCESS_UNBIND == 3,
	       "UNBIND operation moved");
_Static_assert(HERMES_KMS_SESSION_ACCESS_RESULT_BOUND == (1U << 0),
	       "BOUND result bit moved");
_Static_assert(HERMES_KMS_SESSION_ACCESS_RESULT_TOKEN_VALID == (1U << 1),
	       "TOKEN_VALID result bit moved");

ASSERT_SIZE(struct drm_hermes_kms_version, 48);
ASSERT_OFFSET(struct drm_hermes_kms_version, driver_name, 16);

ASSERT_SIZE(struct drm_hermes_kms_caps, 40);
ASSERT_OFFSET(struct drm_hermes_kms_caps, flags, 0);
ASSERT_OFFSET(struct drm_hermes_kms_caps, output_count, 36);

ASSERT_SIZE(struct drm_hermes_kms_status, 208);
ASSERT_OFFSET(struct drm_hermes_kms_status, connector_id, 40);
ASSERT_OFFSET(struct drm_hermes_kms_status, framebuffer_pitch, 100);
ASSERT_OFFSET(struct drm_hermes_kms_status, framebuffer_offset, 116);
ASSERT_OFFSET(struct drm_hermes_kms_status, reserved_alignment, 132);
ASSERT_OFFSET(struct drm_hermes_kms_status, framebuffer_modifier, 136);
ASSERT_OFFSET(struct drm_hermes_kms_status, session_id, 144);
ASSERT_OFFSET(struct drm_hermes_kms_status, owner_pid, 152);
ASSERT_OFFSET(struct drm_hermes_kms_status, bound_fd_count, 160);
ASSERT_OFFSET(struct drm_hermes_kms_status, reserved, 168);

ASSERT_SIZE(struct drm_hermes_kms_identity, 144);
ASSERT_OFFSET(struct drm_hermes_kms_identity, connector_id, 96);
ASSERT_OFFSET(struct drm_hermes_kms_identity, output_index, 112);
ASSERT_OFFSET(struct drm_hermes_kms_identity, device_index, 120);
ASSERT_OFFSET(struct drm_hermes_kms_identity, device_role, 128);
ASSERT_OFFSET(struct drm_hermes_kms_identity, cursor_plane_id, 140);

ASSERT_SIZE(struct drm_hermes_kms_select_output, 80);
ASSERT_OFFSET(struct drm_hermes_kms_select_output, output_name, 16);
ASSERT_OFFSET(struct drm_hermes_kms_select_output, reserved, 48);

ASSERT_SIZE(struct drm_hermes_kms_set_output, 32);
ASSERT_OFFSET(struct drm_hermes_kms_set_output, session_id, 24);

ASSERT_SIZE(struct drm_hermes_kms_acquire_frame, 176);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, framebuffer_id, 32);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, pitch, 52);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, offset, 68);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, dma_buf_fd, 84);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, sync_file_fd, 100);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, damage_x1, 108);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_frame, reserved, 128);

ASSERT_SIZE(struct drm_hermes_kms_wait_frame, 96);
ASSERT_OFFSET(struct drm_hermes_kms_wait_frame, timeout_ms, 40);
ASSERT_OFFSET(struct drm_hermes_kms_wait_frame, reserved, 48);

ASSERT_SIZE(struct drm_hermes_kms_wait_update, 112);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, after_frame_sequence, 8);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, after_cursor_sequence, 16);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, frame_sequence, 24);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, cursor_sequence, 32);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, timeout_ms, 64);
ASSERT_OFFSET(struct drm_hermes_kms_wait_update, reserved, 72);

ASSERT_SIZE(struct drm_hermes_kms_acquire_cursor, 224);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, session_id, 40);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, position_x, 48);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, crtc_x, 56);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, src_x, 72);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, hotspot_x, 88);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, framebuffer_id, 96);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, pitch, 116);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, offset, 132);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, dma_buf_fd, 148);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, sync_file_fd, 164);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, reserved_alignment, 172);
ASSERT_OFFSET(struct drm_hermes_kms_acquire_cursor, reserved, 176);

ASSERT_SIZE(struct drm_hermes_kms_metrics, 312);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, last_update_ns, 128);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, vblank_count, 184);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, vblank_overrun_count, 192);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, bind_count, 200);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, bind_reject_count, 208);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, unbind_count, 216);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, binding_revoke_count, 224);
ASSERT_OFFSET(struct drm_hermes_kms_metrics,
	      cross_session_buffer_export_count, 232);
ASSERT_OFFSET(struct drm_hermes_kms_metrics, reserved, 240);

ASSERT_SIZE(struct drm_hermes_kms_session_access, 72);
ASSERT_OFFSET(struct drm_hermes_kms_session_access, token, 0);
ASSERT_OFFSET(struct drm_hermes_kms_session_access, session_id, 16);
ASSERT_OFFSET(struct drm_hermes_kms_session_access, operation, 24);
ASSERT_OFFSET(struct drm_hermes_kms_session_access, output_index, 28);
ASSERT_OFFSET(struct drm_hermes_kms_session_access, reserved, 40);

ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_GET_VERSION,
	     DRM_HERMES_KMS_GET_VERSION, _IOC_READ, 48, 0x80306440UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_GET_CAPS,
	     DRM_HERMES_KMS_GET_CAPS, _IOC_READ, 40, 0x80286441UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_GET_STATUS,
	     DRM_HERMES_KMS_GET_STATUS, _IOC_READ, 208, 0x80d06442UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_SET_OUTPUT,
	     DRM_HERMES_KMS_SET_OUTPUT, _IOC_READ | _IOC_WRITE, 32,
	     0xc0206443UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_ACQUIRE_FRAME,
	     DRM_HERMES_KMS_ACQUIRE_FRAME, _IOC_READ | _IOC_WRITE, 176,
	     0xc0b06444UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_GET_IDENTITY,
	     DRM_HERMES_KMS_GET_IDENTITY, _IOC_READ, 144, 0x80906445UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_WAIT_FRAME,
	     DRM_HERMES_KMS_WAIT_FRAME, _IOC_READ | _IOC_WRITE, 96,
	     0xc0606446UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_GET_METRICS,
	     DRM_HERMES_KMS_GET_METRICS, _IOC_READ, 312, 0x81386447UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_SELECT_OUTPUT,
	     DRM_HERMES_KMS_SELECT_OUTPUT, _IOC_READ | _IOC_WRITE, 80,
	     0xc0506448UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_SESSION_ACCESS,
	     DRM_HERMES_KMS_SESSION_ACCESS, _IOC_READ | _IOC_WRITE, 72,
	     0xc0486449UL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_ACQUIRE_CURSOR,
	     DRM_HERMES_KMS_ACQUIRE_CURSOR, _IOC_READ | _IOC_WRITE, 224,
	     0xc0e0644aUL);
ASSERT_IOCTL(DRM_IOCTL_HERMES_KMS_WAIT_UPDATE,
	     DRM_HERMES_KMS_WAIT_UPDATE, _IOC_READ | _IOC_WRITE, 112,
	     0xc070644bUL);

int main(void)
{
	return 0;
}
