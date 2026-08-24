/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synthetic EDID construction for Hermes-KMS.
 *
 * Kept separate from the driver, and free of kernel-only constructs, so the
 * same generator that the module compiles can be exercised by a host-side test
 * (tests/edid.c, wired into "make check"). The EDID is easy to get subtly
 * wrong and impossible to notice at runtime -- a bad checksum makes the block
 * vanish, and a too-narrow range descriptor silently filters modes the
 * connector otherwise advertises -- so it is worth pinning down.
 */

#ifndef HERMES_KMS_EDID_H
#define HERMES_KMS_EDID_H

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define DIV_ROUND_UP_ULL(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#endif

#define HERMES_KMS_EDID_SIZE 128

/*
 * Range descriptor byte 4 offset flags and formula byte, mirroring
 * DRM_EDID_RANGE_OFFSET_MAX_HFREQ, DRM_EDID_RANGE_OFFSET_MAX_VFREQ and
 * DRM_EDID_RANGE_LIMITS_ONLY_FLAG from <drm/drm_edid.h>. The driver
 * static_asserts that these still agree with the kernel's own definitions.
 */
#define HERMES_KMS_EDID_OFFSET_MAX_VFREQ 0x02
#define HERMES_KMS_EDID_OFFSET_MAX_HFREQ 0x08
#define HERMES_KMS_EDID_RANGE_LIMITS_ONLY 0x01

/* Byte offsets patched by hermes_kms_build_edid(). */
#define HERMES_KMS_EDID_SERIAL_OFFSET		12
#define HERMES_KMS_EDID_VERSION_OFFSET		18
#define HERMES_KMS_EDID_SIZE_CM_OFFSET		21
#define HERMES_KMS_EDID_FEATURES_OFFSET		24
#define HERMES_KMS_EDID_DTD_OFFSET		54
#define HERMES_KMS_EDID_RANGE_OFFSET		90
#define HERMES_KMS_EDID_CHECKSUM_OFFSET		127

/*
 * CVT keeps the vertical blanking interval at least 550 us long, so the number
 * of lines a mode really needs grows with its refresh rate:
 *
 *	vtotal = vdisplay + 550us * vrefresh * vtotal
 *	       = vdisplay / (1 - 550us * vrefresh)
 *
 * and the horizontal total lands around 1.3x the visible width. Both are
 * estimates, used only to size the advertised range descriptor.
 */
#define HERMES_KMS_CVT_VBLANK_US 550
#define HERMES_KMS_CVT_HTOTAL_NUMERATOR 13
#define HERMES_KMS_CVT_HTOTAL_DENOMINATOR 10

/* Largest values an EDID 1.4 range descriptor can express. */
#define HERMES_KMS_EDID_MAX_RATE_FIELD 255
#define HERMES_KMS_EDID_MAX_HFREQ_KHZ (2 * HERMES_KMS_EDID_MAX_RATE_FIELD)
#define HERMES_KMS_EDID_MAX_VFREQ_HZ (2 * HERMES_KMS_EDID_MAX_RATE_FIELD)

/* Physical size the EDID's millimetre fields can express. */
#define HERMES_KMS_EDID_MAX_SIZE_MM 4095

struct hermes_kms_edid_limits {
	u32 max_width;
	u32 max_height;
	u32 max_refresh_hz;
	u32 physical_width_mm;
	u32 physical_height_mm;
};

/*
 * EDID 1.4 base block identifying the Hermes virtual monitor. Compositors
 * (e.g. KWin) warn and may refuse to configure a connector with no EDID
 * ("Could not find edid for connector"); this block provides identity
 * (manufacturer "HRM", name "Hermes KMS"). The mode list is still generated
 * dynamically in get_modes() via CVT so arbitrary client geometries work, and
 * the detailed timing here is only a fallback/preferred hint.
 *
 * Byte 20 declares a digital input at 8 bits per colour; byte 24 declares
 * RGB 4:4:4 with a preferred timing and, crucially, a continuous-frequency
 * display -- drm_get_monitor_range() ignores the range descriptor entirely
 * without that bit. The range bytes and the checksum below are placeholders
 * that hermes_kms_build_edid() overwrites.
 */
static const u8 hermes_kms_edid_template[HERMES_KMS_EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x22, 0x4d, 0x01, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x22, 0x01, 0x04, 0xa0, 0x00, 0x00, 0x78,
	0x03, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
	0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x13, 0x2b, 0x21, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x48, 0x65, 0x72, 0x6d, 0x65, 0x73, 0x20,
	0x4b, 0x4d, 0x53, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * Rebuild the display range limits descriptor so it covers everything the
 * driver accepts.
 *
 * The range limits are not a hint. Userspace validates modes against them and
 * the kernel infers additional DMT modes through mode_in_range(), so a
 * descriptor narrower than the driver's own envelope silently rules out modes
 * the connector advertises. A fixed block claiming 75 Hz / 150 kHz / 300 MHz
 * once contradicted the driver's 240 Hz ceiling exactly that way: a client
 * asking for 1080p120 got a connector that advertised the mode through CVT and
 * then had it rejected here.
 *
 * EDID 1.3 cannot state more than 255 kHz of horizontal rate. 1.4 adds the
 * +255 offset flags in byte 4, doubling the ceiling, and drm_edid.c honours
 * them in mode_in_hsync_range()/mode_in_vsync_range(). Leaving the formula byte
 * at "range limits only", rather than claiming GTF or CVT, is also what makes
 * drm_get_monitor_range() publish the refresh range as the connector's
 * continuous-frequency range.
 *
 * Returns false when the configured envelope does not fit even in 1.4's range,
 * so the caller can say so instead of leaving a mysteriously filtered mode list
 * behind.
 */
static inline bool
hermes_kms_fill_edid_range(u8 *descriptor,
			   const struct hermes_kms_edid_limits *limits)
{
	u32 vblank_scale = 1000000 -
			   HERMES_KMS_CVT_VBLANK_US * limits->max_refresh_hz;
	u64 vtotal;
	u64 htotal;
	u64 hfreq_khz;
	u64 clock_10mhz;
	u32 vfreq = limits->max_refresh_hz;
	bool representable = true;
	u8 offsets = 0;

	/*
	 * Past ~1818 Hz the blanking model above stops converging. Nothing
	 * sane reaches it, but the arithmetic must not divide by zero or wrap
	 * if a caller ever raises the refresh ceiling that far.
	 */
	if ((u64)HERMES_KMS_CVT_VBLANK_US * limits->max_refresh_hz >= 1000000)
		vblank_scale = 1;

	vtotal = DIV_ROUND_UP_ULL((u64)limits->max_height * 1000000,
				  vblank_scale);
	htotal = DIV_ROUND_UP_ULL((u64)limits->max_width *
				  HERMES_KMS_CVT_HTOTAL_NUMERATOR,
				  HERMES_KMS_CVT_HTOTAL_DENOMINATOR);
	hfreq_khz = DIV_ROUND_UP_ULL(vtotal * limits->max_refresh_hz, 1000);
	clock_10mhz = DIV_ROUND_UP_ULL(hfreq_khz * htotal, 10000);

	if (hfreq_khz > HERMES_KMS_EDID_MAX_HFREQ_KHZ) {
		hfreq_khz = HERMES_KMS_EDID_MAX_HFREQ_KHZ;
		representable = false;
	}
	if (vfreq > HERMES_KMS_EDID_MAX_VFREQ_HZ) {
		vfreq = HERMES_KMS_EDID_MAX_VFREQ_HZ;
		representable = false;
	}
	if (clock_10mhz > HERMES_KMS_EDID_MAX_RATE_FIELD)
		clock_10mhz = HERMES_KMS_EDID_MAX_RATE_FIELD;

	if (hfreq_khz > HERMES_KMS_EDID_MAX_RATE_FIELD) {
		offsets |= HERMES_KMS_EDID_OFFSET_MAX_HFREQ;
		hfreq_khz -= HERMES_KMS_EDID_MAX_RATE_FIELD;
	}
	if (vfreq > HERMES_KMS_EDID_MAX_RATE_FIELD) {
		offsets |= HERMES_KMS_EDID_OFFSET_MAX_VFREQ;
		vfreq -= HERMES_KMS_EDID_MAX_RATE_FIELD;
	}

	descriptor[4] = offsets;
	/*
	 * Advertise a low floor rather than the smallest mode the driver
	 * accepts: the floor only ever excludes modes, and a virtual sink has
	 * no reason to refuse a slow one.
	 */
	descriptor[5] = 1;
	descriptor[6] = (u8)vfreq;
	descriptor[7] = 1;
	descriptor[8] = (u8)hfreq_khz;
	descriptor[9] = (u8)clock_10mhz;
	descriptor[10] = HERMES_KMS_EDID_RANGE_LIMITS_ONLY;

	return representable;
}

/*
 * Build one output's EDID: identity from the template, a distinct serial, the
 * derived range limits, the optionally configured physical size, and a valid
 * checksum. @edid must have room for HERMES_KMS_EDID_SIZE bytes.
 *
 * KWin uses EDID identity when persisting layouts, so identical virtual panels
 * would otherwise be easy to collapse or swap across restarts.
 */
static inline bool
hermes_kms_build_edid(u8 *edid, u32 serial,
		      const struct hermes_kms_edid_limits *limits)
{
	u8 checksum = 0;
	bool representable;
	unsigned int i;

	memcpy(edid, hermes_kms_edid_template, HERMES_KMS_EDID_SIZE);

	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 0] = serial & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 1] = (serial >> 8) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 2] = (serial >> 16) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 3] = (serial >> 24) & 0xff;

	representable = hermes_kms_fill_edid_range(
		&edid[HERMES_KMS_EDID_RANGE_OFFSET], limits);

	/*
	 * The base block states the size in whole centimetres while the
	 * detailed timing states it in millimetres, and userspace reads either,
	 * so write both or leave both at the template's undefined default.
	 */
	if (limits->physical_width_mm && limits->physical_height_mm) {
		u32 width_mm = limits->physical_width_mm;
		u32 height_mm = limits->physical_height_mm;
		u32 width_cm;
		u32 height_cm;

		if (width_mm > HERMES_KMS_EDID_MAX_SIZE_MM)
			width_mm = HERMES_KMS_EDID_MAX_SIZE_MM;
		if (height_mm > HERMES_KMS_EDID_MAX_SIZE_MM)
			height_mm = HERMES_KMS_EDID_MAX_SIZE_MM;
		width_cm = DIV_ROUND_CLOSEST(width_mm, 10);
		height_cm = DIV_ROUND_CLOSEST(height_mm, 10);

		edid[HERMES_KMS_EDID_SIZE_CM_OFFSET + 0] = (u8)width_cm;
		edid[HERMES_KMS_EDID_SIZE_CM_OFFSET + 1] = (u8)height_cm;
		edid[HERMES_KMS_EDID_DTD_OFFSET + 12] = width_mm & 0xff;
		edid[HERMES_KMS_EDID_DTD_OFFSET + 13] = height_mm & 0xff;
		edid[HERMES_KMS_EDID_DTD_OFFSET + 14] =
			(u8)(((width_mm >> 8) << 4) | ((height_mm >> 8) & 0x0f));
	}

	for (i = 0; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		checksum += edid[i];
	edid[HERMES_KMS_EDID_CHECKSUM_OFFSET] = (u8)-checksum;

	return representable;
}

#endif /* HERMES_KMS_EDID_H */
