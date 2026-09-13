/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Write the synthetic EDID the module would publish to stdout, so external
 * EDID parsers can be pointed at it without loading the driver.
 *
 * tests/edid.c pins the bytes the generator is supposed to produce. This is the
 * other half: it hands those same bytes to a real parser (edid-decode, via
 * scripts/check-edid-conformity.sh), which catches the class of mistake a
 * self-consistent generator cannot -- a block that is internally coherent and
 * still not what CTA-861 or EDID 1.4 asked for.
 *
 * It writes exactly hermes_kms_edid_size() bytes, which is the same length the
 * driver hands drm_edid_alloc(), so what a parser sees here is what userspace
 * reads back from the connector's EDID property.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/hermes-kms/hermes_kms_edid.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--hdr] [--generic] [--color-depth N] [--min-width N]\n"
		"          [--min-height N] [--width N] [--height N]\n"
		"          [--refresh N] [--preferred-width N] [--preferred-height N]\n"
		"          [--preferred-refresh N] [--physical-width-mm N]\n"
		"          [--physical-height-mm N]\n"
		"\n"
		"Writes the generated EDID to stdout as raw bytes.\n"
		"Defaults match the module's own defaults with hdr_enable=0.\n",
		argv0);
}

/* Parse a decimal argument, rejecting trailing junk rather than ignoring it. */
static u32 parse_u32(const char *what, const char *value)
{
	char *end;
	unsigned long parsed = strtoul(value, &end, 10);

	if (end == value || *end != '\0' || parsed > 0xffffffffUL) {
		fprintf(stderr, "invalid %s: %s\n", what, value);
		exit(EXIT_FAILURE);
	}
	return (u32)parsed;
}

int main(int argc, char **argv)
{
	u8 edid[2 * HERMES_KMS_EDID_SIZE];
	struct hermes_kms_edid_config config = {
		.max_width = 7680,
		.max_height = 4320,
		.max_refresh_hz = 240,
		.physical_width_mm = 0,
		.physical_height_mm = 0,
		.color_depth = 8,
	};
	bool hdr = false;
	u32 serial = 1;
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value;

		if (!strcmp(arg, "--hdr")) {
			hdr = true;
			continue;
		}
		if (!strcmp(arg, "--generic")) {
			memcpy(config.manufacturer, "VRT", 4);
			memcpy(config.monitor_name, "Virtual KMS", 12);
			serial = 123456;
			continue;
		}
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(argv[0]);
			return EXIT_SUCCESS;
		}

		if (i + 1 >= argc) {
			fprintf(stderr, "%s needs a value\n", arg);
			return EXIT_FAILURE;
		}
		value = argv[++i];

		if (!strcmp(arg, "--color-depth"))
			config.color_depth = parse_u32(arg, value);
		else if (!strcmp(arg, "--min-width"))
			config.min_width = parse_u32(arg, value);
		else if (!strcmp(arg, "--min-height"))
			config.min_height = parse_u32(arg, value);
		else if (!strcmp(arg, "--width"))
			config.max_width = parse_u32(arg, value);
		else if (!strcmp(arg, "--height"))
			config.max_height = parse_u32(arg, value);
		else if (!strcmp(arg, "--refresh"))
			config.max_refresh_hz = parse_u32(arg, value);
		else if (!strcmp(arg, "--preferred-width"))
			config.preferred_width = parse_u32(arg, value);
		else if (!strcmp(arg, "--preferred-height"))
			config.preferred_height = parse_u32(arg, value);
		else if (!strcmp(arg, "--preferred-refresh"))
			config.preferred_refresh_hz = parse_u32(arg, value);
		else if (!strcmp(arg, "--physical-width-mm"))
			config.physical_width_mm = parse_u32(arg, value);
		else if (!strcmp(arg, "--physical-height-mm"))
			config.physical_height_mm = parse_u32(arg, value);
		else {
			fprintf(stderr, "unknown argument: %s\n", arg);
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	hermes_kms_build_edid(edid, serial, &config);

	/*
	 * Mirror the driver: append only when HDR is on, and refuse to emit a
	 * half-built EDID if the append rejects the base, rather than writing
	 * bytes a parser would then be judged against.
	 */
	if (hdr && !hermes_kms_append_hdr_extension(edid)) {
		fprintf(stderr, "refused to append the HDR extension to an invalid base\n");
		return EXIT_FAILURE;
	}

	if (fwrite(edid, 1, hermes_kms_edid_size(edid), stdout) !=
	    hermes_kms_edid_size(edid)) {
		perror("write");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
