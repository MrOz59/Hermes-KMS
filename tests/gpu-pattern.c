/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../tools/hermes-gpu-scanout-test/pattern.h"

int main(void)
{
	/* Fixed vectors catch changes to shader constants, overflow and channels. */
	assert(hermes_gpu_pattern(0, 0, 0) == 0);
	assert(hermes_gpu_pattern(1, 0, 0) == 0xb74efd);
	assert(hermes_gpu_pattern(0, 1, 0) == 0xf89608);
	assert(hermes_gpu_pattern(2559, 1439, 0x3917ab25) == 0x99cd21);
	assert(hermes_gpu_pattern(17, 33, UINT32_MAX) == 0xe396f2);
	const unsigned char vector[] = {0xfd, 0x4e, 0xb7, 255};
	size_t bad = SIZE_MAX;
	assert(hermes_gpu_pattern_verify(vector, 1, 1, 0, &bad) == 1 && bad == 0);
	assert(hermes_gpu_pattern_verify(NULL, 1, 1, 0, &bad) == -1);
	assert(hermes_gpu_pattern_verify(vector, 0, 1, 0, &bad) == -1);
	assert(hermes_gpu_pattern_verify(vector, UINT32_MAX, UINT32_MAX, 0, &bad) == -1);
	const uint32_t w = 2560, h = 1440, seed = 0x3917ab25;
	const size_t bytes = (size_t)w * h * 4;
	unsigned char *pixels = malloc(bytes);
	assert(pixels);
	for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
		uint32_t v = hermes_gpu_pattern(x, y, seed);
		size_t i = ((size_t)y * w + x) * 4;
		pixels[i] = v & 255; pixels[i + 1] = (v >> 8) & 255;
		pixels[i + 2] = (v >> 16) & 255; pixels[i + 3] = 255;
	}
	assert(!hermes_gpu_pattern_verify(pixels, w, h, seed, &bad));
	const size_t corrupt[] = {0, 2 * 1024 * 1024 - 1, 2 * 1024 * 1024,
		2 * 1024 * 1024 + 1, bytes - 1};
	for (size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
		pixels[corrupt[i]] ^= 1;
		assert(hermes_gpu_pattern_verify(pixels, w, h, seed, &bad) == 1);
		assert(bad == corrupt[i]);
		pixels[corrupt[i]] ^= 1;
	}
	/* A wrong seed, vertical origin or channel order must not pass. */
	assert(hermes_gpu_pattern_verify(pixels, w, h, seed + 1, &bad) == 1);
	for (size_t i = 0; i < (size_t)w * 4; i++) {
		unsigned char t = pixels[i]; pixels[i] = pixels[bytes - (size_t)w * 4 + i];
		pixels[bytes - (size_t)w * 4 + i] = t;
	}
	assert(hermes_gpu_pattern_verify(pixels, w, h, seed, &bad) == 1);
	free(pixels);
	puts("GPU pattern: vectors, full-frame and boundary corruption PASS");
	return 0;
}
