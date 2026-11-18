/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HERMES_FRAME_VERIFY_H
#define HERMES_FRAME_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Packed diagnostic snapshots, never the producer's padded allocation size. */
static inline bool hermes_frame_bytes(uint32_t width, uint32_t height,
				      size_t *bytes)
{
	if (!width || !height || !bytes || (size_t)height > SIZE_MAX / width / 4)
		return false;
	*bytes = (size_t)width * height * 4;
	return true;
}

/* GL readback is BGRA; a mapped GL_RGBA8 CUDA array contains RGBA bytes.
 * Compare every byte of the same frozen texture, including alpha. A prefix
 * comparison misses the observed importer corruption beyond roughly 2 MiB.
 * Returns 0 for equal, 1 for mismatch, -1 for invalid snapshot arguments.
 */
static inline int hermes_compare_rgba_bgra(const uint8_t *rgba,
					 const uint8_t *bgra, size_t bytes,
					 size_t *first_mismatch)
{
	static const unsigned swizzle[4] = {2, 1, 0, 3};

	if (!rgba || !bgra || !bytes || bytes % 4 || !first_mismatch)
		return -1;
	for (size_t i = 0; i < bytes; i += 4) {
		for (size_t channel = 0; channel < 4; channel++) {
			if (bgra[i + channel] != rgba[i + swizzle[channel]]) {
				*first_mismatch = i + channel;
				return 1;
			}
		}
	}
	return 0;
}

#endif
