/* SPDX-License-Identifier: GPL-2.0 */
#include "../tools/hermes-egl-import-check/frame_verify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	size_t bytes = 0;
	size_t mismatch = SIZE_MAX;

	assert(!hermes_frame_bytes(0, 1080, &bytes));
	assert(!hermes_frame_bytes(1920, 0, &bytes));
	assert(!hermes_frame_bytes(UINT32_MAX, UINT32_MAX, &bytes));
	assert(!hermes_frame_bytes(1920, 1080, NULL));
	assert(hermes_frame_bytes(2560, 1440, &bytes));
	assert(bytes == (size_t)2560 * 1440 * 4);

	uint8_t *rgba = malloc(bytes);
	uint8_t *bgra = malloc(bytes);
	assert(rgba && bgra);
	for (size_t i = 0; i < bytes; i += 4) {
		/* Nonuniform channels and a pattern that changes across pages. */
		rgba[i] = (uint8_t)((i >> 12) ^ i);
		rgba[i + 1] = (uint8_t)((i >> 16) + 17);
		rgba[i + 2] = (uint8_t)((i >> 8) + 33);
		rgba[i + 3] = (uint8_t)((i >> 20) + 91);
		bgra[i] = rgba[i + 2];
		bgra[i + 1] = rgba[i + 1];
		bgra[i + 2] = rgba[i];
		bgra[i + 3] = rgba[i + 3];
	}
	assert(hermes_compare_rgba_bgra(rgba, bgra, bytes, &mismatch) == 0);
	const size_t offsets[] = {0, (2u << 20) - 1, 2u << 20,
				 (2u << 20) + 3, bytes - 1};
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		const size_t offset = offsets[i];
		bgra[offset] ^= 1;
		assert(hermes_compare_rgba_bgra(rgba, bgra, bytes, &mismatch) == 1);
		assert(mismatch == offset);
		/* The old eight-row test would approve this corrupt suffix. */
		if (offset >= (size_t)2560 * 8 * 4)
			assert(hermes_compare_rgba_bgra(rgba, bgra,
					(size_t)2560 * 8 * 4, &mismatch) == 0);
		bgra[offset] ^= 1;
	}
	assert(hermes_compare_rgba_bgra(rgba, bgra, 3, &mismatch) == -1);
	assert(hermes_compare_rgba_bgra(NULL, bgra, bytes, &mismatch) == -1);
	free(rgba);
	free(bgra);
	puts("full-frame verification: PASS (including corruption beyond 2 MiB)");
	return 0;
}
