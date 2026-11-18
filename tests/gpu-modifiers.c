/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/hermes-gpu-scanout-test/modifiers.h"

/* DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h), including the GB20x
 * split sector-layout field, written out independently of modifiers.h. */
static uint64_t block_linear(unsigned c, unsigned s, unsigned g, unsigned k,
			     unsigned h)
{
	return ((uint64_t)0x03 << 56) |
	       (0x10u | (h & 0xfu) | ((uint64_t)(k & 0xffu) << 12) |
		((uint64_t)(g & 0x3u) << 20) | ((uint64_t)(s & 0x1u) << 22) |
		((uint64_t)(s & 0x6u) << 25) | ((uint64_t)(c & 0x7u) << 23));
}

struct blob_builder {
	unsigned char bytes[512];
	size_t len;
};

/* A version-1 IN_FORMATS blob for formats[] and (mask, offset, modifier). */
static void build_blob(struct blob_builder *b, const uint32_t *formats,
		       uint32_t nformats, const uint64_t (*mods)[3],
		       uint32_t nmods)
{
	uint32_t header[6] = {1, 0, nformats, 24, nmods, 0};

	memset(b, 0, sizeof(*b));
	header[5] = 24 + nformats * 4;
	header[5] = (header[5] + 7) & ~7u;
	memcpy(b->bytes, header, sizeof(header));
	memcpy(b->bytes + 24, formats, nformats * 4);
	for (uint32_t i = 0; i < nmods; i++) {
		unsigned char *e = b->bytes + header[5] + i * 24;
		uint64_t mask = mods[i][0], modifier = mods[i][2];
		uint32_t offset = (uint32_t)mods[i][1], pad = 0;

		memcpy(e, &mask, 8);
		memcpy(e + 8, &offset, 4);
		memcpy(e + 12, &pad, 4);
		memcpy(e + 16, &modifier, 8);
	}
	b->len = header[5] + nmods * 24;
}

int main(void)
{
	const uint64_t bl32 = block_linear(0, 1, 2, 0xfe, 4);
	const uint64_t bl_gb20x_8bpp = block_linear(0, 2, 2, 0xfe, 4);
	const uint64_t bl_compressed = block_linear(1, 1, 2, 0xfe, 4);
	const uint64_t tegra = UINT64_C(0x0300000000000001);
	const uint64_t amd = UINT64_C(0x0200000000000001);
	struct hermes_nvidia_block_linear d;
	char text[160];

	/* Fixed vector against the kernel macro, then field round trips. */
	assert(bl32 == UINT64_C(0x03000000006fe014));
	assert(hermes_mod_decode_nvidia(bl32, &d));
	assert(d.log2_gob_height == 4 && d.page_kind == 0xfe && d.gob_kind == 2 &&
	       d.sector_layout == 1 && d.compression == 0 && !d.reserved_bits_set);
	assert(hermes_mod_decode_nvidia(bl_gb20x_8bpp, &d) && d.sector_layout == 2);
	assert(hermes_mod_decode_nvidia(block_linear(0, 3, 2, 0xfe, 4), &d) &&
	       d.sector_layout == 3);
	assert(hermes_mod_decode_nvidia(bl_compressed, &d) && d.compression == 1);
	assert(hermes_mod_decode_nvidia(bl32 | (UINT64_C(1) << 40), &d) &&
	       d.reserved_bits_set);
	assert(!hermes_mod_decode_nvidia(tegra, &d));
	assert(!hermes_mod_decode_nvidia(amd, &d));
	assert(!hermes_mod_decode_nvidia(HERMES_MOD_LINEAR, &d));
	assert(!hermes_mod_decode_nvidia(HERMES_MOD_INVALID, &d));
	assert(!hermes_mod_decode_nvidia(bl32, NULL));

	hermes_mod_describe(HERMES_MOD_LINEAR, text, sizeof(text));
	assert(!strcmp(text, "linear"));
	hermes_mod_describe(bl_gb20x_8bpp, text, sizeof(text));
	assert(strstr(text, "16-GOB") && strstr(text, "GB20x"));
	hermes_mod_describe(tegra, text, sizeof(text));
	assert(strstr(text, "NVIDIA vendor layout"));
	hermes_mod_describe(amd, text, 8);
	assert(strlen(text) == 7);

	/* Intersection keeps the first list's order and drops INVALID/dupes. */
	const uint64_t render[] = {bl32, HERMES_MOD_INVALID, bl_compressed,
				   HERMES_MOD_LINEAR, bl32};
	const uint64_t plane_default[] = {HERMES_MOD_LINEAR};
	const uint64_t plane_tuned[] = {HERMES_MOD_LINEAR, bl_compressed, bl32,
					HERMES_MOD_INVALID};
	uint64_t out[8];
	size_t total = 0;
	assert(hermes_mod_intersect(render, 5, plane_default, 1, out, 8, &total) == 1 &&
	       total == 1 && out[0] == HERMES_MOD_LINEAR);
	assert(hermes_mod_intersect(render, 5, plane_tuned, 4, out, 8, &total) == 3 &&
	       out[0] == bl32 && out[1] == bl_compressed && out[2] == HERMES_MOD_LINEAR);
	assert(hermes_mod_intersect(render, 5, plane_tuned, 4, out, 1, &total) == 1 &&
	       total == 3);

	/* Suggestions exclude linear/INVALID, dedupe and respect the limit. */
	assert(hermes_mod_suggest_scanout(render, 5, out, &total) == 2 &&
	       out[0] == bl32 && out[1] == bl_compressed && total == 0);
	uint64_t many[20], suggested[HERMES_MAX_EXTRA_SCANOUT_MODIFIERS];
	for (unsigned i = 0; i < 20; i++)
		many[i] = block_linear(i % 2, 1, 2, 0x70 + i, 4);
	assert(hermes_mod_suggest_scanout(many, 20, suggested, &total) ==
		       HERMES_MAX_EXTRA_SCANOUT_MODIFIERS &&
	       total == 5);

	/* IN_FORMATS: per-format masks, offsets, and malformed blobs. */
	const uint32_t formats[] = {0x34325258 /* XR24 */, 0x34325241 /* AR24 */,
				    0x30335241 /* AR30 */};
	const uint64_t mods[][3] = {
		{0x7, 0, HERMES_MOD_LINEAR}, /* every format */
		{0x2, 0, bl32},              /* AR24 only */
		{0x1, 1, bl_compressed},     /* offset 1: AR24 */
		{0x1, 2, amd},               /* offset 2: AR30 */
	};
	struct blob_builder blob;
	build_blob(&blob, formats, 3, mods, 4);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len, 0x34325241, out, 8) == 3 &&
	       out[0] == HERMES_MOD_LINEAR && out[1] == bl32 && out[2] == bl_compressed);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len, 0x34325258, out, 8) == 1 &&
	       out[0] == HERMES_MOD_LINEAR);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len, 0x30335241, out, 8) == 2 &&
	       out[1] == amd);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len, 0x3231564e, out, 8) == 0);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len, 0x34325241, out, 1) == 3);
	assert(hermes_in_formats_modifiers(blob.bytes, blob.len - 1, 0x34325241, out, 8) == -1);
	assert(hermes_in_formats_modifiers(blob.bytes, 23, 0x34325241, out, 8) == -1);
	assert(hermes_in_formats_modifiers(NULL, blob.len, 0x34325241, out, 8) == -1);
	unsigned char bad[sizeof(blob.bytes)];
	memcpy(bad, blob.bytes, sizeof(bad));
	bad[0] = 2; /* unknown version */
	assert(hermes_in_formats_modifiers(bad, blob.len, 0x34325241, out, 8) == -1);
	memcpy(bad, blob.bytes, sizeof(bad));
	uint32_t huge = UINT32_MAX;
	memcpy(bad + 16, &huge, 4); /* count_modifiers past the end */
	assert(hermes_in_formats_modifiers(bad, blob.len, 0x34325241, out, 8) == -1);
	memcpy(bad, blob.bytes, sizeof(bad));
	memcpy(bad + 12, &huge, 4); /* formats_offset past the end */
	assert(hermes_in_formats_modifiers(bad, blob.len, 0x34325241, out, 8) == -1);

	puts("GPU modifiers: NVIDIA decode, KWin-style intersection, suggestions, IN_FORMATS PASS");
	return 0;
}
