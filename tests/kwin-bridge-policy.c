/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tools/hermes-kms-kwin-bridge/bridge_policy.h"

static void check_strip(const char *in, const char *expected)
{
	char out[256];

	assert(hkb_strip_preload(in, out, sizeof(out)));
	if (strcmp(out, expected) != 0) {
		fprintf(stderr, "strip(\"%s\") = \"%s\", expected \"%s\"\n",
			in ? in : "(null)", out, expected);
		assert(0);
	}
}

int main(void)
{
	struct hkb_pci_address a, b;
	unsigned int maj, min;
	char small[4];

	/* PCI addresses */
	assert(hkb_parse_pci("0000:01:00.0", &a));
	assert(a.domain == 0 && a.bus == 1 && a.dev == 0 && a.func == 0);
	assert(hkb_parse_pci("000a:ff:1f.7", &b));
	assert(b.domain == 0xa && b.bus == 0xff && b.dev == 0x1f && b.func == 7);
	assert(!hkb_parse_pci("", &a));
	assert(!hkb_parse_pci(NULL, &a));
	assert(!hkb_parse_pci("01:00.0", &a));
	assert(!hkb_parse_pci("0000:01:00.0x", &a));
	assert(!hkb_parse_pci("0000:01:20.0", &a));
	assert(!hkb_parse_pci("0000:01:00.8", &a));
	assert(!hkb_parse_pci("10000:01:00.0", &a));
	assert(!hkb_parse_pci("0x00:01:00.0", &a));
	assert(!hkb_parse_pci("0000:0x1:00.0", &a));
	assert(!hkb_parse_pci("0000:01:00.", &a));
	assert(!hkb_parse_pci("0000::00.0", &a));
	assert(!hkb_parse_pci(" 0000:01:00.0", &a));
	assert(!hkb_parse_pci("0000:01:00.0 ", &a));
	assert(hkb_parse_pci("0:1:0.0", &a) && a.bus == 1);
	assert(hkb_parse_pci("0000:0A:1F.7", &a) && a.bus == 0xa && a.dev == 0x1f);
	assert(hkb_parse_pci("0000:01:00.0", &a));
	assert(hkb_parse_pci("0000:03:00.0", &b));
	assert(hkb_pci_less(&a, &b) && !hkb_pci_less(&b, &a));
	assert(!hkb_pci_equal(&a, &b) && hkb_pci_equal(&a, &a));
	assert(hkb_parse_pci("0000:01:00.1", &b));
	assert(hkb_pci_less(&a, &b));
	assert(hkb_parse_pci("0001:00:00.0", &b));
	assert(hkb_pci_less(&a, &b));

	/* Mode */
	assert(hkb_parse_mode(NULL) == HKB_MODE_AUTO);
	assert(hkb_parse_mode("") == HKB_MODE_AUTO);
	assert(hkb_parse_mode("auto") == HKB_MODE_AUTO);
	assert(hkb_parse_mode("1") == HKB_MODE_AUTO);
	assert(hkb_parse_mode("off") == HKB_MODE_OFF);
	assert(hkb_parse_mode("0") == HKB_MODE_OFF);
	assert(hkb_parse_mode("no") == HKB_MODE_OFF);
	assert(hkb_parse_mode("force") == HKB_MODE_FORCE);
	assert(hkb_parse_mode("yes please") == HKB_MODE_INVALID);

	/* KWin library names */
	assert(hkb_parse_kwin_library("libkwin.so.6.8.4", &maj, &min));
	assert(maj == 6 && min == 8);
	assert(hkb_parse_kwin_library("libkwin.so.6.10", &maj, &min));
	assert(maj == 6 && min == 10);
	assert(!hkb_parse_kwin_library("libkwin.so.6", &maj, &min));
	assert(!hkb_parse_kwin_library("libkwin.so.6.8.4x", &maj, &min));
	assert(!hkb_parse_kwin_library("libkwin.so.6.8.4.1", &maj, &min));
	assert(!hkb_parse_kwin_library("libkwineffects.so.6.8.4", &maj, &min));
	assert(!hkb_parse_kwin_library(NULL, &maj, &min));
	assert(hkb_kwin_release_reviewed(6, 8));
	assert(!hkb_kwin_release_reviewed(6, 7));
	assert(!hkb_kwin_release_reviewed(6, 9));
	assert(!hkb_kwin_release_reviewed(7, 8));

	/* Callers */
	assert(hkb_is_libkwin_name("/usr/lib/libkwin.so.6"));
	assert(hkb_is_libkwin_name("libkwin.so.6.8.4"));
	assert(!hkb_is_libkwin_name("/usr/lib/libkwineffects.so.6"));
	assert(!hkb_is_libkwin_name("/usr/lib/libgbm.so.1"));
	assert(!hkb_is_libkwin_name(NULL));

	/* LD_PRELOAD */
	check_strip(NULL, "");
	check_strip("", "");
	check_strip(HKB_LIBRARY_NAME, "");
	check_strip("/usr/lib/" HKB_LIBRARY_NAME, "");
	check_strip("liba.so:" HKB_LIBRARY_NAME " libb.so", "liba.so:libb.so");
	check_strip("  liba.so  ", "liba.so");
	check_strip(HKB_LIBRARY_NAME ".1", HKB_LIBRARY_NAME ".1");
	check_strip("x" HKB_LIBRARY_NAME, "x" HKB_LIBRARY_NAME);
	assert(!hkb_strip_preload("liblong.so", small, sizeof(small)));
	assert(!hkb_strip_preload("", small, 0));

	puts("kwin-bridge-policy: ok");
	return 0;
}
