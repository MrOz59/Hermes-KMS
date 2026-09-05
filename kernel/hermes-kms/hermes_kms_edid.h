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
 * The DTD offset equals 4 (the CTA header) plus the byte length of the data
 * block collection: an HDR Static Metadata block (4 bytes) followed by a
 * Colorimetry block (4 bytes) gives 4 + 8 = 12. With no detailed timings, this
 * also marks where the zero padding begins.
 */
#define HERMES_KMS_CTA_TAG		0x02	/* extension block[0] */
#define HERMES_KMS_CTA_REVISION		0x03	/* extension block[1] */
#define HERMES_KMS_CTA_DTD_OFFSET	0x0c	/* extension block[2]: 4 + 8-byte collection */

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

/* Physical size the EDID's millimetre fields can express. */
#define HERMES_KMS_EDID_MAX_SIZE_MM 4095

struct hermes_kms_edid_config {
	u32 max_width;
	u32 max_height;
	u32 max_refresh_hz;
	u32 physical_width_mm;
	u32 physical_height_mm;
	/* Bits per primary colour channel; 0 leaves the depth undefined. */
	u32 color_depth;
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

	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 0] = serial & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 1] = (serial >> 8) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 2] = (serial >> 16) & 0xff;
	edid[HERMES_KMS_EDID_SERIAL_OFFSET + 3] = (serial >> 24) & 0xff;

	representable = hermes_kms_fill_edid_range(
		&edid[HERMES_KMS_EDID_RANGE_OFFSET], config);

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
 * property. So this block carries two data blocks back to back: the HDR
 * Static Metadata block then the Colorimetry block. The CTA header advertises
 * no native video formats and a DTD offset of 12, which spans the CTA header
 * plus both 4-byte data blocks; everything after byte 12 is zero padding to
 * the checksum.
 *
 * The EOTF byte is written from a single combined constant that always sets
 * the traditional-SDR-gamma bit together with the SMPTE ST2084 (PQ) bit, so
 * the block can never advertise PQ without a defined SDR fallback -- the one
 * cross-flag invariant CTA-861.3 expects. The Colorimetry block sets only the
 * BT2020 RGB bit and no gamut-metadata bits. The checksum at offset 127 is
 * chosen so all 128 bytes sum to 0 mod 256, matching the base block's own
 * checksum rule; without it the block is silently dropped by EDID parsers.
 */
static inline void hermes_kms_build_edid_hdr_extension(u8 *block)
{
	u8 checksum = 0;
	unsigned int i;

	/* CTA-861 extension header. */
	block[0] = HERMES_KMS_CTA_TAG;
	block[1] = HERMES_KMS_CTA_REVISION;
	block[2] = HERMES_KMS_CTA_DTD_OFFSET;
	block[3] = 0x00; /* no native formats, no flags */

	/*
	 * HDR Static Metadata Data Block (bytes 4..7): a use-extended-tag data
	 * block whose tag/length byte selects the extended-tag type (top three
	 * bits 0x07) and declares three payload bytes following it, the
	 * extended tag identifies it as HDR Static Metadata (0x06), the EOTF
	 * byte offers SDR gamma and ST2084/PQ together, and the descriptor byte
	 * declares Static Metadata Descriptor Type 1.
	 */
	block[4] = (u8)((HERMES_KMS_CTA_TAG_EXTENDED << 5) | 3);
	block[5] = HERMES_KMS_CTA_EXT_TAG_HDR_SM;
	block[6] = HERMES_KMS_HDR_EOTF_SDR_GAMMA | HERMES_KMS_HDR_EOTF_ST2084_PQ;
	block[7] = HERMES_KMS_HDR_SM_TYPE1;

	/*
	 * Colorimetry Data Block (bytes 8..11): another use-extended-tag data
	 * block with three payload bytes; the extended tag identifies it as
	 * Colorimetry (0x05), the colorimetry byte sets only the BT2020 RGB bit,
	 * and the final gamut-metadata byte sets no bits.
	 */
	block[8] = (u8)((HERMES_KMS_CTA_TAG_EXTENDED << 5) | 3);
	block[9] = HERMES_KMS_CTA_EXT_TAG_COLORIMETRY;
	block[10] = HERMES_KMS_COLORIMETRY_BT2020_RGB;
	block[11] = 0x00; /* no gamut-metadata bits */

	/* Zero the padding region up to the checksum byte. */
	for (i = HERMES_KMS_CTA_DTD_OFFSET; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		block[i] = 0x00;

	for (i = 0; i < HERMES_KMS_EDID_CHECKSUM_OFFSET; i++)
		checksum += block[i];
	block[HERMES_KMS_EDID_CHECKSUM_OFFSET] = (u8)-checksum;
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

	hermes_kms_build_edid_hdr_extension(&edid[HERMES_KMS_EDID_SIZE]);

	return true;
}

#endif /* HERMES_KMS_EDID_H */
