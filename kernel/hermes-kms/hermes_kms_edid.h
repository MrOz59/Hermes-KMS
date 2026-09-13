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
#define HERMES_KMS_EDID_VIDEO_INPUT_OFFSET	20
#define HERMES_KMS_EDID_SIZE_CM_OFFSET		21
#define HERMES_KMS_EDID_FEATURES_OFFSET		24
#define HERMES_KMS_EDID_DTD_OFFSET		54
#define HERMES_KMS_EDID_RANGE_OFFSET		90
#define HERMES_KMS_EDID_EXT_COUNT_OFFSET	126
#define HERMES_KMS_EDID_CHECKSUM_OFFSET		127

/*
 * CTA-861 extension header, carried in the second EDID block. A compositor
 * treats an output as HDR-capable only when both an EDID HDR Static Metadata
 * Data Block and the HDR_OUTPUT_METADATA connector property are present, and
 * KWin additionally gates HDR behind a BT2020 Colorimetry Data Block, so the
 * extension carries both data blocks. Tag 0x02 marks a CTA extension; the DTD
 * offset says "no detailed timings" and points past the data block collection.
 *
 * With a native 1080p60 DTD, the DTD offset equals 4 (the CTA header) plus
 * a 3-byte Video Data Block, 3-byte VCDB, 4-byte HDR Static Metadata block
 * and 4-byte Colorimetry block: 18. Other preferred timings omit VIC 16 and
 * use a 17-byte offset.
 */
#define HERMES_KMS_CTA_TAG		0x02	/* extension block[0] */
#define HERMES_KMS_CTA_REVISION		0x03	/* extension block[1] */
#define HERMES_KMS_CTA_DTD_OFFSET	0x12	/* extension block[2]: 4 + 14-byte collection */

/*
 * The HDR Static Metadata Data Block is a CTA "use extended tag" data block:
 * the top three bits of its tag/length byte select the extended-tag block
 * type (0x07), and the extended tag byte that follows (0x06) identifies it as
 * HDR Static Metadata (CTA-861.3).
 */
#define HERMES_KMS_CTA_TAG_EXTENDED	0x07	/* tag/length byte, bits 7:5 */
#define HERMES_KMS_CTA_EXT_TAG_HDR_SM	0x06	/* HDR static metadata */

/*
 * The Colorimetry Data Block is another CTA "use extended tag" data block
 * (extended tag 0x05). Its colorimetry byte advertises supported extended
 * colorimetry encodings; bit 7 signals BT2020 RGB, which is what makes KWin's
 * edid()->supportsBT2020() return true.
 */
#define HERMES_KMS_CTA_EXT_TAG_COLORIMETRY	0x05	/* colorimetry data block */
#define HERMES_KMS_COLORIMETRY_BT2020_RGB	0x80	/* colorimetry byte, bit 7 */

/*
 * EOTF support flags in the HDR Static Metadata Data Block. Traditional SDR
 * gamma and SMPTE ST2084 (PQ) are advertised together so PQ is never offered
 * without a defined SDR fallback; HERMES_KMS_HDR_SM_TYPE1 declares support for
 * Static Metadata Descriptor Type 1.
 */
#define HERMES_KMS_HDR_EOTF_SDR_GAMMA	0x01	/* traditional gamma SDR */
#define HERMES_KMS_HDR_EOTF_ST2084_PQ	0x04	/* SMPTE ST2084 (PQ) */
#define HERMES_KMS_HDR_SM_TYPE1		0x01	/* Static Metadata Descriptor Type 1 */

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

/* Largest physical size both the centimetre and DTD fields can express. */
#define HERMES_KMS_EDID_MAX_SIZE_MM 2550

struct hermes_kms_edid_config {
	u32 min_width;
	u32 min_height;
	u32 max_width;
	u32 max_height;
	u32 max_refresh_hz;
	u32 preferred_width;
	u32 preferred_height;
	u32 preferred_refresh_hz;
	u32 physical_width_mm;
	u32 physical_height_mm;
	/* Bits per primary colour channel; 0 leaves the depth undefined. */
	u32 color_depth;
	/* Optional three-letter EISA ID and printable monitor name (max 12). */
	char manufacturer[4];
	char monitor_name[14];
};

/*
 * Byte 20 encodes the colour bit depth in bits 6:4 as an index rather than a
 * count: 0 undefined, 1 six bits, 2 eight, 3 ten, 4 twelve, 5 fourteen, 6
 * sixteen. A compositor reads this to decide whether driving the output at ten
 * bits per channel is worth doing at all, so advertising more than eight stays
 * a deliberate configuration choice rather than a default.
 */
static inline u8 hermes_kms_edid_depth_field(u32 color_depth)
{
	switch (color_depth) {
	case 6:
		return 1;
	case 8:
		return 2;
	case 10:
		return 3;
	case 12:
		return 4;
	case 14:
		return 5;
	case 16:
		return 6;
	default:
		return 0;
	}
}

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
		0x07, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, 0x00,
	0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
	0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0x13, 0x2b, 0x21, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x48, 0x65, 0x72, 0x6d, 0x65, 0x73, 0x20,
	0x4b, 0x4d, 0x53, 0x0a, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static inline bool hermes_kms_edid_mode_supported(
	const struct hermes_kms_edid_config *config, u32 width, u32 height,
	u32 refresh_hz)
{
	return width >= config->min_width && width <= config->max_width &&
	       height >= config->min_height && height <= config->max_height &&
	       refresh_hz && refresh_hz <= config->max_refresh_hz;
}

/* Encode a valid fallback timing for profiles that cannot use 1080p60. */
static inline u32 hermes_kms_edid_hblank(u32 width)
{
	u32 blank = ((width / 5 + 7) / 8) * 8;

	return blank < 160 ? 160 : blank;
}

static inline bool hermes_kms_edid_write_dtd(u8 *dtd, u32 width, u32 height,
					      u32 refresh_hz)
{
	u32 hblank, vblank = 45, hfront, hsync;
	u64 clock_10khz;

	if (!width || !height || width > 4095 || height > 4095 || !refresh_hz)
		return false;
	hblank = hermes_kms_edid_hblank(width);
	hfront = hblank / 3;
	hsync = hblank / 3;
	clock_10khz = ((u64)(width + hblank) * (height + vblank) *
			 refresh_hz + 5000) / 10000;
	if (clock_10khz < 1000 || clock_10khz > 65535)
		return false;

	memset(dtd, 0, 18);
	dtd[0] = (u8)clock_10khz;
	dtd[1] = (u8)(clock_10khz >> 8);
	dtd[2] = (u8)width;
	dtd[3] = (u8)hblank;
	dtd[4] = (u8)(((width >> 8) << 4) | (hblank >> 8));
	dtd[5] = (u8)height;
	dtd[6] = (u8)vblank;
	dtd[7] = (u8)(((height >> 8) << 4) | (vblank >> 8));
	dtd[8] = (u8)hfront;
	dtd[9] = (u8)hsync;
	dtd[10] = 0x35; /* vertical front porch 3, sync pulse 5 */
	dtd[11] = (u8)(((hfront >> 8) << 6) | ((hsync >> 8) << 4));
	dtd[17] = 0x1e; /* non-interlaced, separate positive sync */
	return true;
}

static inline void hermes_kms_edid_set_preferred_timing(
	u8 *edid, const struct hermes_kms_edid_config *config)
{
	u32 width = config->preferred_width;
	u32 height = config->preferred_height;
	u32 refresh_hz = config->preferred_refresh_hz;
	u8 *dtd = &edid[HERMES_KMS_EDID_DTD_OFFSET];

	if (hermes_kms_edid_mode_supported(config, width, height, refresh_hz) &&
	    width == 1920 && height == 1080 && refresh_hz == 60)
		return; /* preserve the standard CTA timing in the template */
	if (hermes_kms_edid_mode_supported(config, width, height, refresh_hz) &&
	    hermes_kms_edid_write_dtd(dtd, width, height, refresh_hz))
		return;
	if (hermes_kms_edid_mode_supported(config, 1920, 1080, 60))
		return;

	width = config->max_width < 4095 ? config->max_width : 4095;
	height = config->max_height < 4095 ? config->max_height : 4095;
	if (width >= config->min_width && height >= config->min_height) {
		u64 pixels = (u64)(width + hermes_kms_edid_hblank(width)) *
			     (height + 45);
		u32 min_refresh = (u32)((10000000 + pixels - 1) / pixels);
		u32 max_refresh = (u32)(655350000 / pixels);

		refresh_hz = config->max_refresh_hz < 60 ?
			config->max_refresh_hz : 60;
		if (refresh_hz < min_refresh)
			refresh_hz = min_refresh;
		if (refresh_hz > max_refresh)
			refresh_hz = max_refresh;
		if (refresh_hz <= config->max_refresh_hz &&
		    hermes_kms_edid_write_dtd(dtd, width, height, refresh_hz))
			return;
	}

	/* EDID DTDs cannot encode every legal KMS mode (e.g. 8K-only cards). */
	memcpy(dtd, &hermes_kms_edid_template[108], 18);
	edid[24] &= (u8)~0x02; /* no preferred detailed timing */
}

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
			   const struct hermes_kms_edid_config *config)
{
	u32 vblank_scale = 1000000 -
			   HERMES_KMS_CVT_VBLANK_US * config->max_refresh_hz;
	u64 vtotal;
	u64 htotal;
	u64 hfreq_khz;
	u64 clock_10mhz;
	u32 vfreq = config->max_refresh_hz;
	bool representable = true;
	u8 offsets = 0;

	/*
	 * Past ~1818 Hz the blanking model above stops converging. Nothing
	 * sane reaches it, but the arithmetic must not divide by zero or wrap
	 * if a caller ever raises the refresh ceiling that far.
	 */
	if ((u64)HERMES_KMS_CVT_VBLANK_US * config->max_refresh_hz >= 1000000)
		vblank_scale = 1;

	vtotal = DIV_ROUND_UP_ULL((u64)config->max_height * 1000000,
				  vblank_scale);
	htotal = DIV_ROUND_UP_ULL((u64)config->max_width *
				  HERMES_KMS_CVT_HTOTAL_NUMERATOR,
				  HERMES_KMS_CVT_HTOTAL_DENOMINATOR);
	hfreq_khz = DIV_ROUND_UP_ULL(vtotal * config->max_refresh_hz, 1000);
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
		      const struct hermes_kms_edid_config *config)
{
	u8 checksum = 0;
	bool representable;
	unsigned int i;

	memcpy(edid, hermes_kms_edid_template, HERMES_KMS_EDID_SIZE);
	hermes_kms_edid_set_preferred_timing(edid, config);
	/* Avoid claiming a physical size in the DTD when none was configured. */
	edid[HERMES_KMS_EDID_DTD_OFFSET + 12] = 0;
	edid[HERMES_KMS_EDID_DTD_OFFSET + 13] = 0;
	edid[HERMES_KMS_EDID_DTD_OFFSET + 14] = 0;
	/* Advertise VGA only when the card can actually scan it out. */
	if (hermes_kms_edid_mode_supported(config, 640, 480, 60))
		edid[35] |= 0x20;
	if (config->manufacturer[0]) {
		u32 code = ((u32)(config->manufacturer[0] - 'A' + 1) << 10) |
			   ((u32)(config->manufacturer[1] - 'A' + 1) << 5) |
			   (u32)(config->manufacturer[2] - 'A' + 1);

		edid[8] = code >> 8;
		edid[9] = code & 0xff;
	}
	if (config->monitor_name[0]) {
		unsigned int n = 0;

		memset(&edid[77], ' ', 13);
		while (n < 12 && config->monitor_name[n]) {
			edid[77 + n] = config->monitor_name[n];
			n++;
		}
		edid[77 + n] = '\n';
	}

	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 0] = serial & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 1] = (serial >> 8) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 2] = (serial >> 16) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 3] = (serial >> 24) & 0xff;

	representable = hermes_kms_fill_edid_range(
		&edid[HERMES_KMS_EDID_RANGE_OFFSET], config);
	if (edid[HERMES_KMS_EDID_DTD_OFFSET] ||
	    edid[HERMES_KMS_EDID_DTD_OFFSET + 1]) {
		const u8 *dtd = &edid[HERMES_KMS_EDID_DTD_OFFSET];
		u8 *range = &edid[HERMES_KMS_EDID_RANGE_OFFSET];
		u32 htotal = dtd[2] | ((dtd[4] & 0xf0) << 4);
		u32 hfreq_khz;
		u32 encoded_max = range[8] +
			((range[4] & HERMES_KMS_EDID_OFFSET_MAX_HFREQ) ? 255 : 0);

		htotal += dtd[3] | ((dtd[4] & 0x0f) << 8);
		hfreq_khz = ((u32)(dtd[0] | (dtd[1] << 8)) * 10 +
			      htotal - 1) / htotal;
		if (hfreq_khz > encoded_max &&
		    hfreq_khz <= HERMES_KMS_EDID_MAX_HFREQ_KHZ) {
			if (hfreq_khz > 255) {
				range[4] |= HERMES_KMS_EDID_OFFSET_MAX_HFREQ;
				hfreq_khz -= 255;
			}
			range[8] = (u8)hfreq_khz;
		}
	}

	/* Digital input, interface undefined; only the depth field varies. */
	edid[HERMES_KMS_EDID_VIDEO_INPUT_OFFSET] =
		(u8)(0x80 | (hermes_kms_edid_depth_field(config->color_depth) << 4));

	/*
	 * The base block states the size in whole centimetres while the
	 * detailed timing states it in millimetres, and userspace reads either,
	 * so write both or leave both at the template's undefined default.
	 */
	if (config->physical_width_mm && config->physical_height_mm) {
		u32 width_mm = config->physical_width_mm;
		u32 height_mm = config->physical_height_mm;
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
		if (edid[HERMES_KMS_EDID_DTD_OFFSET] ||
		    edid[HERMES_KMS_EDID_DTD_OFFSET + 1]) {
			edid[HERMES_KMS_EDID_DTD_OFFSET + 12] = width_mm & 0xff;
			edid[HERMES_KMS_EDID_DTD_OFFSET + 13] = height_mm & 0xff;
			edid[HERMES_KMS_EDID_DTD_OFFSET + 14] =
				(u8)(((width_mm >> 8) << 4) |
				     ((height_mm >> 8) & 0x0f));
		}
	}

	for (i = 0; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		checksum += edid[i];
	edid[HERMES_KMS_EDID_CHECKSUM_OFFSET] = (u8)-checksum;

	return representable;
}

/*
 * Build the CTA-861 extension block (the second EDID block) carrying an HDR
 * Static Metadata Data Block into @block, which must have room for
 * HERMES_KMS_EDID_SIZE bytes.
 *
 * A compositor only treats an output as HDR-capable when it finds an HDR
 * Static Metadata Data Block in the EDID, and KWin's WideColorGamut gate --
 * a prerequisite it requires before it will enable HDR at all -- additionally
 * needs edid()->supportsBT2020() to return true, which comes from a BT2020
 * Colorimetry Data Block, working alongside the connector's Colorspace
 * property. The extension carries baseline VIC 1 and a VCDB for selectable
 * RGB quantization. VIC 16 is included when the base DTD is the template's
 * 1080p60 timing. The CTA header declares underscan and the corresponding
 * DTD offset, followed by zero padding.
 *
 * The EOTF byte is written from a single combined constant that always sets
 * the traditional-SDR-gamma bit together with the SMPTE ST2084 (PQ) bit, so
 * the block can never advertise PQ without a defined SDR fallback -- the one
 * cross-flag invariant CTA-861.3 expects. The Colorimetry block sets only the
 * BT2020 RGB bit and no gamut-metadata bits. The checksum at offset 127 is
 * chosen so all 128 bytes sum to 0 mod 256, matching the base block's own
 * checksum rule; without it the block is silently dropped by EDID parsers.
 */
static inline void hermes_kms_build_edid_hdr_extension_for_mode(
	u8 *block, bool include_vic16)
{
	u8 checksum = 0;
	unsigned int i, at;

	/* CTA-861 extension header. */
	block[0] = HERMES_KMS_CTA_TAG;
	block[1] = HERMES_KMS_CTA_REVISION;
	block[3] = 0x80; /* IT video formats are underscanned by default */

	/* VIC 16 is present only when the base DTD really is 1080p60. */
	block[4] = include_vic16 ? 0x42 : 0x41;
	at = 5;
	if (include_vic16)
		block[at++] = 0x90;
	block[at++] = 0x01; /* required baseline 640x480p60 */

	/* RGB quantization range is selectable (VCDB, extended tag 0). */
	block[at++] = 0xe2;
	block[at++] = 0x00;
	block[at++] = 0x4a; /* selectable RGB; IT/CE formats underscanned */

	/*
	 * HDR Static Metadata Data Block: a use-extended-tag data
	 * block whose tag/length byte selects the extended-tag type (top three
	 * bits 0x07) and declares three payload bytes following it, the
	 * extended tag identifies it as HDR Static Metadata (0x06), the EOTF
	 * byte offers SDR gamma and ST2084/PQ together, and the descriptor byte
	 * declares Static Metadata Descriptor Type 1.
	 */
	block[at++] = (u8)((HERMES_KMS_CTA_TAG_EXTENDED << 5) | 3);
	block[at++] = HERMES_KMS_CTA_EXT_TAG_HDR_SM;
	block[at++] = HERMES_KMS_HDR_EOTF_SDR_GAMMA | HERMES_KMS_HDR_EOTF_ST2084_PQ;
	block[at++] = HERMES_KMS_HDR_SM_TYPE1;

	/*
	 * Colorimetry Data Block: another use-extended-tag data
	 * block with three payload bytes; the extended tag identifies it as
	 * Colorimetry (0x05), the colorimetry byte sets only the BT2020 RGB bit,
	 * and the final gamut-metadata byte sets no bits.
	 */
	block[at++] = (u8)((HERMES_KMS_CTA_TAG_EXTENDED << 5) | 3);
	block[at++] = HERMES_KMS_CTA_EXT_TAG_COLORIMETRY;
	block[at++] = HERMES_KMS_COLORIMETRY_BT2020_RGB;
	block[at++] = 0x00; /* no gamut-metadata bits */
	block[2] = (u8)at;

	/* Zero the padding region up to the checksum byte. */
	for (i = at; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		block[i] = 0x00;

	for (i = 0; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		checksum += block[i];
	block[HERMES_KMS_EDID_CHECKSUM_OFFSET] = (u8)-checksum;
}

static inline void hermes_kms_build_edid_hdr_extension(u8 *block)
{
	hermes_kms_build_edid_hdr_extension_for_mode(block, true);
}

/*
 * Promote a finalized single-block base EDID in @edid into a two-block EDID
 * that advertises HDR: flip the base block's extension-count byte to 1, fix up
 * the base checksum, and build the CTA extension block into the second block.
 * @edid must have room for 2 * HERMES_KMS_EDID_SIZE bytes.
 *
 * The base block's extension-count byte (offset 126) tells the EDID parser how
 * many trailing blocks to read, so it must move from 0 to 1 in lockstep with
 * the checksum: raising byte 126 by one raises the running sum by one, so the
 * checksum byte drops by exactly one to keep all 128 bytes summing to 0 mod
 * 256. Recomputing (rather than blindly decrementing) keeps this correct even
 * if the base block ever changes, and it stays byte-for-byte equal to
 * "old checksum - 1 mod 256" because that is the only value that preserves the
 * invariant.
 *
 * Guard first: a valid base block already sums to 0 mod 256, and appending an
 * extension to a broken base would only produce a broken two-block EDID. So if
 * the incoming base does not already sum to 0, refuse -- return false and
 * leave the extension region (edid[128..255]) untouched -- rather than
 * "finalizing" garbage.
 */
static inline bool hermes_kms_append_hdr_extension(u8 *edid)
{
	u8 sum = 0;
	unsigned int i;

	for (i = 0; i < HERMES_KMS_EDID_SIZE; i++)
		sum += edid[i];
	if (sum != 0)
		return false;

	edid[HERMES_KMS_EDID_EXT_COUNT_OFFSET] = 1;

	sum = 0;
	for (i = 0; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		sum += edid[i];
	edid[HERMES_KMS_EDID_CHECKSUM_OFFSET] = (u8)-sum;

	hermes_kms_build_edid_hdr_extension_for_mode(
		&edid[HERMES_KMS_EDID_SIZE],
		memcmp(&edid[HERMES_KMS_EDID_DTD_OFFSET],
		       &hermes_kms_edid_template[HERMES_KMS_EDID_DTD_OFFSET],
		       12) == 0);

	return true;
}

/*
 * The number of EDID bytes to publish for @edid: one block, plus one more for
 * each block the base block's extension-count byte declares.
 *
 * This exists because the two lengths in play are not the same. The buffer is
 * sized for the largest EDID the driver can build (base plus one CTA
 * extension), but the EDID it actually built may be shorter. DRM limits *mode
 * parsing* using the extension count, so an oversized buffer parses correctly
 * either way -- but drm_edid_alloc() publishes exactly the number of bytes it
 * is handed, so passing the buffer capacity would put 128 trailing zero bytes
 * into the blob userspace reads back from the connector's EDID property, in a
 * blob whose own base block says there is no second block. edid-decode and
 * anything else parsing that property sees the contradiction.
 *
 * Derive the length from the extension-count byte rather than tracking it in a
 * separate field, so the published length and the block the EDID claims to
 * have cannot drift apart. That also makes the failure path correct for free:
 * when hermes_kms_append_hdr_extension() refuses an invalid base it leaves the
 * count at 0, and this keeps reporting a single block.
 *
 * The count is only ever 0 (the template's own value) or 1 (written by the
 * append helper), so the result never exceeds a two-block buffer.
 */
static inline unsigned int hermes_kms_edid_size(const u8 *edid)
{
	return HERMES_KMS_EDID_SIZE *
	       (1u + edid[HERMES_KMS_EDID_EXT_COUNT_OFFSET]);
}

#endif /* HERMES_KMS_EDID_H */
