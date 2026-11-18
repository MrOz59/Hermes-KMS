/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GPU-free regression for tools/hermes_nvidia_version.h: the tools stamp every
 * hardware report with the loaded NVIDIA module, so the parser must find the
 * version and flavour in every known line shape and reject everything else.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../tools/hermes_nvidia_version.h"

static void expect(const char *line, unsigned int major, unsigned int minor,
		   unsigned int patch, int open, const char *text)
{
	struct hermes_nvidia_version version;

	if (hermes_nvidia_parse_version_line(line, &version) ||
	    version.major != major || version.minor != minor ||
	    version.patch != patch || version.open != open ||
	    strcmp(version.text, text)) {
		fprintf(stderr, "FAIL: %s\n", line);
		exit(1);
	}
}

static void reject(const char *line)
{
	struct hermes_nvidia_version version;

	if (!hermes_nvidia_parse_version_line(line, &version)) {
		fprintf(stderr, "FAIL: accepted %s\n", line ? line : "(null)");
		exit(1);
	}
	assert(version.major == 0 && version.text[0] == '\0');
}

int main(void)
{
	expect("NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  "
	       "Release Build  (dvs-builder@U22-I3-AM21-27-1)  Thu Aug 14 01:00:00 UTC 2026\n",
	       610, 43, 3, 1, "610.43.03");
	expect("NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.178.04  Tue Sep 23 2026\n",
	       580, 178, 4, 0, "580.178.04");
	expect("NVRM version: NVIDIA UNIX Open Kernel Module for aarch64  595.91.07  Release Build\n",
	       595, 91, 7, 1, "595.91.07");
	/* Two-component releases (older branches and some betas). */
	expect("NVRM version: NVIDIA UNIX x86_64 Kernel Module  560.35  Fri Aug 16 2024",
	       560, 35, 0, 0, "560.35");
	/* Tabs and no trailing newline. */
	expect("NVRM version:\tNVIDIA UNIX Open Kernel Module for x86_64\t615.71.09",
	       615, 71, 9, 1, "615.71.09");

	reject(NULL);
	reject("");
	reject("GCC version:  gcc version 15.2.1 20250813 (GCC)\n");
	reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module\n");
	reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  610.43.03.1  build\n");
	reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  610.  build\n");
	reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  .43  build\n");
	reject("NVRM version: NVIDIA UNIX x86_64 Kernel Module  99999999999.1  build\n");

	/* The file reader: a missing file is "not loaded", not an error exit. */
	{
		struct hermes_nvidia_version version;
		char path[] = "/tmp/hermes-nvidia-version.XXXXXX";
		int fd = mkstemp(path);
		static const char contents[] =
			"NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.57.04  Release Build\n"
			"GCC version:  gcc version 15.2.1 (GCC)\n";

		assert(fd >= 0);
		assert(write(fd, contents, sizeof(contents) - 1) == (ssize_t)(sizeof(contents) - 1));
		close(fd);
		assert(!hermes_nvidia_read_version(path, &version));
		assert(version.major == 610 && version.minor == 57 && version.patch == 4 && version.open);
		unlink(path);
		assert(hermes_nvidia_read_version(path, &version) == -1);
	}

	printf("NVIDIA version parsing: PASS\n");
	return 0;
}
