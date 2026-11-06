/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Which NVIDIA kernel module is loaded, for the diagnostic tools' reports.
 *
 * NVIDIA's DMA-BUF import, mapping and teardown behaviour has changed between
 * releases (the 2026-09-30 security bulletin alone fixed several bugs on those
 * paths), so a hardware verdict is only meaningful next to the exact driver
 * that produced it. The first line of /proc/driver/nvidia/version names both
 * the version and the flavour:
 *
 *   NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  610.43.03  Release Build  (...)
 *   NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.82.07  Sun Aug 17 ...
 */
#ifndef HERMES_NVIDIA_VERSION_H
#define HERMES_NVIDIA_VERSION_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HERMES_NVIDIA_VERSION_PATH "/proc/driver/nvidia/version"

struct hermes_nvidia_version {
	unsigned int major;
	unsigned int minor;
	unsigned int patch;	/* 0 when the release has two components */
	int open;		/* 1 for the open kernel module, 0 proprietary */
	char text[32];		/* the version token as printed by the driver */
};

/* Parse "N.N" or "N.N.N" occupying all of [token, token + length). */
static inline int hermes_nvidia_parse_token(const char *token, size_t length,
					    struct hermes_nvidia_version *out)
{
	unsigned int parts[3] = {0, 0, 0};
	unsigned int count = 0;
	int digits = 0;

	if (!length || length >= sizeof(out->text))
		return -1;
	for (size_t i = 0; i < length; i++) {
		const char c = token[i];

		if (isdigit((unsigned char)c)) {
			if (parts[count] > 100000)
				return -1;
			parts[count] = parts[count] * 10 + (unsigned int)(c - '0');
			digits = 1;
		} else if (c == '.' && digits && count < 2) {
			count++;
			digits = 0;
		} else {
			return -1;
		}
	}
	if (!digits || count < 1)
		return -1;
	out->major = parts[0];
	out->minor = parts[1];
	out->patch = parts[2];
	memcpy(out->text, token, length);
	out->text[length] = '\0';
	return 0;
}

/*
 * Parse the first line of /proc/driver/nvidia/version. The version is the
 * first whitespace-separated token made only of dotted numbers, so neither the
 * architecture ("x86_64") nor the build host or date can be mistaken for it.
 */
static inline int hermes_nvidia_parse_version_line(const char *line,
						   struct hermes_nvidia_version *out)
{
	static const char prefix[] = "NVRM version:";
	const char *p;

	memset(out, 0, sizeof(*out));
	if (!line || strncmp(line, prefix, sizeof(prefix) - 1))
		return -1;
	p = line + sizeof(prefix) - 1;
	while (*p && *p != '\n') {
		const char *start;

		while (*p == ' ' || *p == '\t')
			p++;
		start = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		if (p > start &&
		    !hermes_nvidia_parse_token(start, (size_t)(p - start), out)) {
			out->open = strstr(line, "Open Kernel Module") != NULL;
			return 0;
		}
	}
	memset(out, 0, sizeof(*out));
	return -1;
}

/* 0 on success; -1 when no NVIDIA module is loaded or the line is unreadable. */
static inline int hermes_nvidia_read_version(const char *path,
					     struct hermes_nvidia_version *out)
{
	char line[512];
	FILE *file = fopen(path ? path : HERMES_NVIDIA_VERSION_PATH, "re");
	int ret = -1;

	memset(out, 0, sizeof(*out));
	if (!file)
		return -1;
	if (fgets(line, sizeof(line), file))
		ret = hermes_nvidia_parse_version_line(line, out);
	fclose(file);
	return ret;
}

/*
 * One line for a tool's report, so a verdict pasted into an issue always
 * carries the driver it was produced on.
 */
static inline void hermes_nvidia_print_version(FILE *stream)
{
	struct hermes_nvidia_version version;

	if (hermes_nvidia_read_version(NULL, &version))
		fprintf(stream, "nvidia driver: not loaded (%s unreadable)\n",
			HERMES_NVIDIA_VERSION_PATH);
	else
		fprintf(stream, "nvidia driver: %s (%s kernel module)\n",
			version.text, version.open ? "open" : "proprietary");
}

#endif /* HERMES_NVIDIA_VERSION_H */
