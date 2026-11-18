/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Hermes-KMS contributors */
#ifndef HERMES_KMS_KWIN_BRIDGE_POLICY_H
#define HERMES_KMS_KWIN_BRIDGE_POLICY_H

/*
 * Pure decisions of the KWin render bridge, kept free of libdrm, GBM and
 * process state so `make check` can test them without a GPU or a compositor.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HKB_LIBRARY_NAME "libhermes-kms-kwin-bridge.so"
#define HKB_KMS_DRIVER_NAME "hermes-kms"
#define HKB_NVIDIA_VENDOR_ID 0x10deu

struct hkb_pci_address {
	uint16_t domain;
	uint8_t bus;
	uint8_t dev;
	uint8_t func;
};

/* Reads 1..max_digits hex digits (no sign, no "0x") followed by end. */
static inline const char *hkb_parse_hex_field(const char *text,
					      unsigned int max_digits,
					      char end, unsigned int *value)
{
	unsigned int digits = 0, result = 0;

	for (; *text && *text != end; text++, digits++) {
		char c = *text;
		unsigned int nibble;

		if (c >= '0' && c <= '9')
			nibble = (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')
			nibble = (unsigned int)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			nibble = (unsigned int)(c - 'A' + 10);
		else
			return NULL;
		if (digits == max_digits)
			return NULL;
		result = result * 16 + nibble;
	}
	if (digits == 0 || *text != end)
		return NULL;
	*value = result;
	return end ? text + 1 : text;
}

/* Parses "DDDD:BB:DD.F" (hex, as sysfs and lspci -D print it). */
static inline bool hkb_parse_pci(const char *text, struct hkb_pci_address *out)
{
	unsigned int domain, bus, dev, func;

	if (!text)
		return false;
	text = hkb_parse_hex_field(text, 4, ':', &domain);
	if (text)
		text = hkb_parse_hex_field(text, 2, ':', &bus);
	if (text)
		text = hkb_parse_hex_field(text, 2, '.', &dev);
	if (text)
		text = hkb_parse_hex_field(text, 1, '\0', &func);
	if (!text)
		return false;
	if (dev > 0x1f || func > 7)
		return false;
	out->domain = (uint16_t)domain;
	out->bus = (uint8_t)bus;
	out->dev = (uint8_t)dev;
	out->func = (uint8_t)func;
	return true;
}

static inline bool hkb_pci_equal(const struct hkb_pci_address *a,
				 const struct hkb_pci_address *b)
{
	return a->domain == b->domain && a->bus == b->bus &&
	       a->dev == b->dev && a->func == b->func;
}

static inline bool hkb_pci_less(const struct hkb_pci_address *a,
				const struct hkb_pci_address *b)
{
	if (a->domain != b->domain)
		return a->domain < b->domain;
	if (a->bus != b->bus)
		return a->bus < b->bus;
	if (a->dev != b->dev)
		return a->dev < b->dev;
	return a->func < b->func;
}

/* HERMES_KMS_KWIN_BRIDGE: unset/""/"auto" = auto, "off" family, "force". */
enum hkb_mode {
	HKB_MODE_AUTO,
	HKB_MODE_OFF,
	HKB_MODE_FORCE,
	HKB_MODE_INVALID,
};

static inline enum hkb_mode hkb_parse_mode(const char *value)
{
	if (!value || !*value || strcmp(value, "auto") == 0 ||
	    strcmp(value, "1") == 0 || strcmp(value, "on") == 0)
		return HKB_MODE_AUTO;
	if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0 ||
	    strcmp(value, "false") == 0 || strcmp(value, "no") == 0)
		return HKB_MODE_OFF;
	if (strcmp(value, "force") == 0)
		return HKB_MODE_FORCE;
	return HKB_MODE_INVALID;
}

/*
 * The KWin library's real file name ("libkwin.so.6.8.4") carries the release
 * whose allocator and render-device behaviour this bridge was reviewed
 * against. Returns false for anything that is not libkwin.so.MAJOR.MINOR[.PATCH].
 */
static inline bool hkb_parse_kwin_library(const char *basename,
					  unsigned int *major,
					  unsigned int *minor)
{
	static const char prefix[] = "libkwin.so.";
	unsigned int maj, min, patch;
	char trailing;
	int fields;

	if (!basename || strncmp(basename, prefix, sizeof(prefix) - 1) != 0)
		return false;
	basename += sizeof(prefix) - 1;
	fields = sscanf(basename, "%u.%u.%u%c", &maj, &min, &patch, &trailing);
	if (fields != 3) {
		fields = sscanf(basename, "%u.%u%c", &maj, &min, &trailing);
		if (fields != 2)
			return false;
	}
	*major = maj;
	*minor = min;
	return true;
}

/*
 * KWin releases whose DRM backend was read for this bridge: 6.8 opens every
 * KMS GPU's allocator through gbm_create_device(), allocates dumb buffers
 * through gbm_device_get_fd(), and asks GpuManager::compatibleRenderDevice()
 * which render device serves a KMS device. Anything else needs `force`.
 */
static inline bool hkb_kwin_release_reviewed(unsigned int major,
					     unsigned int minor)
{
	return major == 6 && minor == 8;
}

/* Whether a shared object's file name is KWin's core library. */
static inline bool hkb_is_libkwin_name(const char *path)
{
	const char *base;

	if (!path)
		return false;
	base = strrchr(path, '/');
	base = base ? base + 1 : path;
	return strncmp(base, "libkwin.so", 10) == 0;
}

/*
 * Removes every occurrence of HKB_LIBRARY_NAME (bare or as a path) from an
 * LD_PRELOAD list, which ld.so separates by colons or spaces. Writes the rest,
 * joined by single colons, into out. Returns false when out is too small.
 */
static inline bool hkb_strip_preload(const char *value, char *out,
				     size_t out_size)
{
	size_t used = 0;
	const char *cursor = value ? value : "";

	if (out_size == 0)
		return false;
	out[0] = '\0';
	while (*cursor) {
		const char *end, *base;
		size_t length;

		while (*cursor == ':' || *cursor == ' ')
			cursor++;
		if (!*cursor)
			break;
		end = cursor;
		while (*end && *end != ':' && *end != ' ')
			end++;
		length = (size_t)(end - cursor);
		base = cursor;
		for (const char *p = cursor; p < end; p++) {
			if (*p == '/')
				base = p + 1;
		}
		if (!((size_t)(end - base) == sizeof(HKB_LIBRARY_NAME) - 1 &&
		      memcmp(base, HKB_LIBRARY_NAME,
			     sizeof(HKB_LIBRARY_NAME) - 1) == 0)) {
			size_t needed = length + (used ? 1 : 0);

			if (used + needed + 1 > out_size)
				return false;
			if (used)
				out[used++] = ':';
			memcpy(out + used, cursor, length);
			used += length;
			out[used] = '\0';
		}
		cursor = end;
	}
	return true;
}

#endif /* HERMES_KMS_KWIN_BRIDGE_POLICY_H */
