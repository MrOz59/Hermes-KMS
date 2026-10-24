/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HERMES_GPU_MODIFIERS_H
#define HERMES_GPU_MODIFIERS_H

/*
 * Format-modifier helpers for the GPU-origin experiment, without libdrm, EGL
 * or a GPU so tests/gpu-modifiers.c can pin their behaviour.
 *
 * The question they answer is which layouts a compositor such as KWin can
 * choose for a Hermes-KMS output. When it renders on another GPU and imports
 * that buffer into the output's device, KWin intersects the output plane's
 * IN_FORMATS with the render GPU's non-external-only modifiers. Hermes-KMS
 * accepts any modifier at ADDFB2 but advertises only linear unless
 * scanout_modifiers= lists more, so this intersection decides whether the
 * scanout can stay in the GPU's own tiled layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HERMES_MOD_LINEAR UINT64_C(0)
#define HERMES_MOD_INVALID UINT64_C(0x00ffffffffffffff)
#define HERMES_MOD_VENDOR_NVIDIA 0x03u
/* Kernel limit for the scanout_modifiers= module parameter. */
#define HERMES_MAX_EXTRA_SCANOUT_MODIFIERS 15u

static inline unsigned int hermes_mod_vendor(uint64_t modifier)
{
	return (unsigned int)(modifier >> 56);
}

/* DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(): vendor 0x03 with bit 4 set. */
static inline bool hermes_mod_is_nvidia_block_linear(uint64_t modifier)
{
	return hermes_mod_vendor(modifier) == HERMES_MOD_VENDOR_NVIDIA &&
	       (modifier & 0x10u);
}

struct hermes_nvidia_block_linear {
	unsigned int log2_gob_height; /* h, bits 3:0 */
	unsigned int page_kind;       /* k, bits 19:12 */
	unsigned int gob_kind;        /* g, bits 21:20 */
	unsigned int sector_layout;   /* s, bit 22 plus bits 27:26 (GB20x) */
	unsigned int compression;     /* c, bits 25:23 */
	bool reserved_bits_set;       /* bits 55:28, 11:5 must be zero */
};

/*
 * Decode a block-linear modifier. The sector layout gained bits 27:26 for
 * GB20x (Blackwell) 8/16-bpp surfaces in "drm: define NVIDIA DRM format
 * modifiers for GB20x"; older decoders that read only bit 22 report 0 or 1
 * for those, so both fields are part of the identity of a layout.
 */
static inline bool hermes_mod_decode_nvidia(uint64_t modifier,
					    struct hermes_nvidia_block_linear *out)
{
	if (!out || !hermes_mod_is_nvidia_block_linear(modifier))
		return false;
	out->log2_gob_height = (unsigned int)(modifier & 0xfu);
	out->page_kind = (unsigned int)((modifier >> 12) & 0xffu);
	out->gob_kind = (unsigned int)((modifier >> 20) & 0x3u);
	out->sector_layout = (unsigned int)(((modifier >> 22) & 0x1u) |
					    (((modifier >> 26) & 0x3u) << 1));
	out->compression = (unsigned int)((modifier >> 23) & 0x7u);
	out->reserved_bits_set = (modifier & UINT64_C(0x00fffffff0000fe0)) != 0;
	return true;
}

/* A short human-readable description; always NUL-terminates `buf`. */
static inline void hermes_mod_describe(uint64_t modifier, char *buf, size_t len)
{
	struct hermes_nvidia_block_linear bl;

	if (!buf || !len)
		return;
	if (modifier == HERMES_MOD_LINEAR) {
		snprintf(buf, len, "linear");
	} else if (modifier == HERMES_MOD_INVALID) {
		snprintf(buf, len, "invalid (implicit layout)");
	} else if (hermes_mod_decode_nvidia(modifier, &bl)) {
		snprintf(buf, len,
			 "NVIDIA block-linear: %u-GOB blocks, page kind 0x%02x, gob kind %u, sector layout %u%s, compression %u%s",
			 1u << bl.log2_gob_height, bl.page_kind, bl.gob_kind,
			 bl.sector_layout,
			 bl.sector_layout >= 2 ? " (GB20x 8/16 bpp)" : "",
			 bl.compression,
			 bl.reserved_bits_set ? ", reserved bits set" : "");
	} else if (hermes_mod_vendor(modifier) == HERMES_MOD_VENDOR_NVIDIA) {
		snprintf(buf, len, "NVIDIA vendor layout 0x%014llx",
			 (unsigned long long)(modifier & UINT64_C(0x00ffffffffffffff)));
	} else {
		snprintf(buf, len, "vendor 0x%02x layout 0x%014llx",
			 hermes_mod_vendor(modifier),
			 (unsigned long long)(modifier & UINT64_C(0x00ffffffffffffff)));
	}
}

static inline bool hermes_mod_contains(const uint64_t *list, size_t count,
				       uint64_t modifier)
{
	for (size_t i = 0; i < count; i++)
		if (list[i] == modifier)
			return true;
	return false;
}

/*
 * Entries of `a` that also appear in `b`, in `a`'s order, without duplicates
 * and never DRM_FORMAT_MOD_INVALID. Returns how many were written (at most
 * `cap`); `*total` (optional) receives how many there were in all.
 */
static inline size_t hermes_mod_intersect(const uint64_t *a, size_t na,
					  const uint64_t *b, size_t nb,
					  uint64_t *out, size_t cap,
					  size_t *total)
{
	size_t written = 0, found = 0;

	for (size_t i = 0; i < na; i++) {
		if (a[i] == HERMES_MOD_INVALID || !hermes_mod_contains(b, nb, a[i]))
			continue;
		bool seen = false;
		for (size_t j = 0; j < i; j++)
			if (a[j] == a[i])
				seen = true;
		if (seen)
			continue;
		found++;
		if (written < cap && out)
			out[written++] = a[i];
	}
	if (total)
		*total = found;
	return written;
}

/*
 * The extra layouts to publish through scanout_modifiers=: every modifier the
 * render GPU can render into (non-external-only), except linear (always
 * advertised) and INVALID, deduplicated, in the driver's preference order,
 * truncated to the kernel's limit. `*dropped` receives how many did not fit.
 */
static inline size_t hermes_mod_suggest_scanout(const uint64_t *render,
						size_t count, uint64_t *out,
						size_t *dropped)
{
	size_t written = 0, skipped = 0;

	for (size_t i = 0; i < count; i++) {
		if (render[i] == HERMES_MOD_LINEAR || render[i] == HERMES_MOD_INVALID ||
		    hermes_mod_contains(out, written, render[i]))
			continue;
		bool repeated = false;
		for (size_t j = 0; j < i; j++)
			if (render[j] == render[i])
				repeated = true;
		if (repeated)
			continue;
		if (written < HERMES_MAX_EXTRA_SCANOUT_MODIFIERS)
			out[written++] = render[i];
		else
			skipped++;
	}
	if (dropped)
		*dropped = skipped;
	return written;
}

/*
 * IN_FORMATS blob (struct drm_format_modifier_blob, version 1), parsed with
 * explicit bounds checks and memcpy so a truncated or misaligned blob from a
 * buggy driver cannot be over-read. Layout, all native endian:
 *   u32 version, flags, count_formats, formats_offset,
 *       count_modifiers, modifiers_offset;
 *   u32 formats[count_formats] at formats_offset;
 *   { u64 formats; u32 offset; u32 pad; u64 modifier; }[count_modifiers]
 *       at modifiers_offset, where bit i of `formats` covers formats[offset+i].
 * Writes the modifiers listed for `format` (at most `cap`) and returns how
 * many there are in all, or -1 for a malformed blob.
 */
static inline long hermes_in_formats_modifiers(const void *blob, size_t len,
					       uint32_t format, uint64_t *out,
					       size_t cap)
{
	const unsigned char *base = blob;
	uint32_t header[6];
	size_t total = 0, written = 0;
	uint32_t format_index = UINT32_MAX;

	if (!blob || len < sizeof(header))
		return -1;
	memcpy(header, base, sizeof(header));
	const uint32_t version = header[0], count_formats = header[2];
	const uint32_t formats_offset = header[3], count_modifiers = header[4];
	const uint32_t modifiers_offset = header[5];
	if (version != 1 ||
	    (size_t)formats_offset > len ||
	    (size_t)count_formats > (len - formats_offset) / sizeof(uint32_t) ||
	    (size_t)modifiers_offset > len ||
	    (size_t)count_modifiers > (len - modifiers_offset) / 24u)
		return -1;
	for (uint32_t i = 0; i < count_formats; i++) {
		uint32_t f;

		memcpy(&f, base + formats_offset + (size_t)i * sizeof(f), sizeof(f));
		if (f == format) {
			format_index = i;
			break;
		}
	}
	if (format_index == UINT32_MAX)
		return 0;
	for (uint32_t i = 0; i < count_modifiers; i++) {
		const unsigned char *entry = base + modifiers_offset + (size_t)i * 24u;
		uint64_t mask, modifier;
		uint32_t offset;

		memcpy(&mask, entry, sizeof(mask));
		memcpy(&offset, entry + 8, sizeof(offset));
		memcpy(&modifier, entry + 16, sizeof(modifier));
		if (format_index < offset || format_index - offset >= 64u ||
		    !((mask >> (format_index - offset)) & 1u))
			continue;
		total++;
		if (written < cap && out)
			out[written++] = modifier;
	}
	return (long)total;
}

#endif
