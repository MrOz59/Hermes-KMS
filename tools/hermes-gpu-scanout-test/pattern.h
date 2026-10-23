/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HERMES_GPU_PATTERN_H
#define HERMES_GPU_PATTERN_H

#include <stddef.h>
#include <stdint.h>

/* Same unsigned 32-bit arithmetic as the GLSL shader. Coordinates are GL's
 * bottom-left origin. Every row/page has different data; alpha is opaque. */
static inline uint32_t hermes_gpu_pattern(uint32_t x, uint32_t y, uint32_t seed)
{
	uint32_t v = x * UINT32_C(1664525) + y * UINT32_C(1013904223) + seed;
	v ^= v >> 16;
	v *= UINT32_C(2246822519);
	v ^= v >> 13;
	return v & UINT32_C(0x00ffffff);
}

/* Packed RGBA bytes, without alignment or host-endian assumptions. */
static inline int hermes_gpu_pattern_verify(const unsigned char *rgba,
	uint32_t width, uint32_t height, uint32_t seed, size_t *bad_byte)
{
	if (!rgba || !width || !height || (size_t)width > SIZE_MAX / 4 / height)
		return -1;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t v = hermes_gpu_pattern(x, y, seed);
			size_t i = ((size_t)y * width + x) * 4;
			for (unsigned int c = 0; c < 4; c++) {
				unsigned char expected = c == 3 ? 255 : (v >> (c * 8)) & 255;
				if (rgba[i + c] != expected) {
					if (bad_byte) *bad_byte = i + c;
					return 1;
				}
			}
		}
	}
	return 0;
}
#endif
