KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
PWD := $(shell pwd)

# Build the module with the same toolchain the target kernel was built with.
# A clang-built kernel (e.g. CachyOS) needs LLVM=1; a gcc-built kernel must not
# set it. Detect from the kernel config so DKMS works across distros; callers
# can still override LLVM explicitly.
ifeq ($(origin LLVM), undefined)
LLVM := $(shell grep -qx 'CONFIG_CC_IS_CLANG=y' $(KDIR)/.config 2>/dev/null && echo 1 || echo 0)
endif

# Normalize the value passed to recursive kbuild. GNU make otherwise forwards
# a caller's command-line LLVM=0 through MAKEFLAGS even when it is omitted from
# the recipe, and current kbuild rejects 0. An explicit empty LLVM= overrides
# that inherited value and selects the normal GCC toolchain. Non-empty Clang
# selectors (LLVM=1, LLVM=-18, or a tool-prefix path) are forwarded unchanged.
ifeq ($(LLVM),0)
LLVM_FLAG := LLVM=
else ifneq ($(strip $(LLVM)),)
LLVM_FLAG := LLVM=$(LLVM)
else
LLVM_FLAG := LLVM=
endif

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
UAPI_CFLAGS := -I$(PWD)/include/uapi
IMPORT_CHECK_CFLAGS := $(shell pkg-config --cflags libva libva-drm libdrm 2>/dev/null)
IMPORT_CHECK_LIBS := $(shell pkg-config --libs libva libva-drm libdrm 2>/dev/null)
EGL_CHECK_CFLAGS := $(shell pkg-config --cflags libdrm gbm egl gl 2>/dev/null)
EGL_CHECK_LIBS := $(shell pkg-config --libs libdrm gbm egl gl 2>/dev/null)

# The CUDA stage of the EGL checker is opt-in: the tool compiles the stage out
# when HAVE_CUDA is not set, so hosts without the CUDA toolkit still build the
# full tools target. Enable with `make tools HAVE_CUDA=1` (needs cuda.h,
# cudaGL.h and libcuda; override CUDA_CFLAGS/CUDA_LIBS for non-default
# toolkit locations).
HAVE_CUDA ?= 0
CUDA_CFLAGS ?=
CUDA_LIBS ?= -lcuda
ifeq ($(HAVE_CUDA),1)
EGL_CHECK_CUDA_CFLAGS := -DHAVE_CUDA $(CUDA_CFLAGS)
EGL_CHECK_CUDA_LIBS := $(CUDA_LIBS)
else
EGL_CHECK_CUDA_CFLAGS :=
EGL_CHECK_CUDA_LIBS :=
endif
CURSOR_PROBE_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null)
CURSOR_PROBE_LIBS := $(shell pkg-config --libs libdrm 2>/dev/null)
UDEV_RULE_DIR ?= /etc/udev/rules.d
SYSTEM_UDEV_RULE_DIR ?= /usr/lib/udev/rules.d
PREFIX ?= /usr
UAPI_INCLUDE_DIR ?= $(PREFIX)/include/drm
HERMES_INCLUDE_DIR ?= $(PREFIX)/include/hermes-kms
HERMES_LICENSE_DIR ?= $(PREFIX)/share/licenses/hermes-kms

DKMS_NAME := hermes-kms
DKMS_VERSION := $(shell awk '/^#define HERMES_KMS_DRIVER_MAJOR/{maj=$$3} /^#define HERMES_KMS_DRIVER_MINOR/{min=$$3} /^#define HERMES_KMS_DRIVER_PATCH/{pat=$$3} END{print maj"."min"."pat}' kernel/hermes-kms/hermes_kms.c)
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

.PHONY: all full check check-uapi check-session check-edid modules tools install-runtime-udev uninstall-runtime-udev \
	install-dev-udev uninstall-dev-udev \
	dkms-install dkms-uninstall modules-install install-configs \
	install-uapi uninstall-uapi clean clean-tools

all: modules

# Opt in to developer diagnostics that need libdrm, VAAPI, GBM, EGL and GL.
full: modules tools

check: check-uapi check-session check-edid

# Pin public structure layouts and ioctl encodings on the native userspace ABI.
# When a working multilib compiler is present, exercise the 32-bit compat ABI
# too; an actual ABI assertion failure remains fatal rather than being skipped.
check-uapi:
	@set -eu; \
		test -n '$(CURSOR_PROBE_CFLAGS)' || { \
			printf 'missing libdrm pkg-config metadata\n' >&2; \
			exit 1; \
		}; \
		test_tmp="$$(mktemp -d "$${TMPDIR:-/tmp}/hermes-kms-uapi.XXXXXX")"; \
		cleanup() { $(RM) -r -- "$$test_tmp"; }; \
		trap cleanup EXIT; \
		trap 'exit 1' HUP INT TERM; \
		$(CC) $(CFLAGS) -std=c11 -Werror -pedantic \
			$(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) \
			tests/uapi-abi.c -o "$$test_tmp/uapi-abi-native"; \
		"$$test_tmp/uapi-abi-native"; \
		printf 'UAPI ABI: native PASS\n'; \
		if printf 'int main(void) { return 0; }\n' | \
			$(CC) -m32 -x c - -o "$$test_tmp/m32-probe" >/dev/null 2>&1; then \
			$(CC) -m32 $(CFLAGS) -std=c11 -Werror -pedantic \
				$(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) \
				tests/uapi-abi.c -o "$$test_tmp/uapi-abi-32"; \
			"$$test_tmp/uapi-abi-32"; \
			printf 'UAPI ABI: 32-bit PASS\n'; \
		else \
			printf 'UAPI ABI: 32-bit SKIP (multilib compiler/runtime unavailable)\n'; \
			fi

# Exercise the file-backed CLI credential transport without loading the driver.
check-session:
	@set -eu; \
		test -n '$(CURSOR_PROBE_CFLAGS)' || { \
			printf 'missing libdrm pkg-config metadata\n' >&2; \
			exit 1; \
		}; \
		test_tmp="$$(mktemp -d "$${TMPDIR:-/tmp}/hermes-kms-session.XXXXXX")"; \
		cleanup() { $(RM) -r -- "$$test_tmp"; }; \
		trap cleanup EXIT; \
		trap 'exit 1' HUP INT TERM; \
		$(CC) $(CFLAGS) -std=c11 -Werror -pedantic \
			$(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) \
			tests/session-file.c -o "$$test_tmp/session-file"; \
		"$$test_tmp/session-file"; \
		printf 'session file helper: native PASS\n'; \
		if printf 'int main(void) { return 0; }\n' | \
			$(CC) -m32 -x c - -o "$$test_tmp/m32-probe" >/dev/null 2>&1; then \
			$(CC) -m32 $(CFLAGS) -std=c11 -Werror -pedantic \
				$(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) \
				tests/session-file.c -o "$$test_tmp/session-file-32"; \
			"$$test_tmp/session-file-32"; \
			printf 'session file helper: 32-bit PASS\n'; \
		else \
			printf 'session file helper: 32-bit SKIP (multilib compiler/runtime unavailable)\n'; \
		fi

# Validate the synthetic EDID the module hands to compositors. A bad checksum
# makes the block vanish and a too-narrow range descriptor silently filters
# advertised modes, so both are pinned here rather than discovered on a desktop.
# Needs no libdrm: the generator header is deliberately free of kernel-only
# constructs so it can be compiled on the host.
check-edid:
	@set -eu; \
		test_tmp="$$(mktemp -d "$${TMPDIR:-/tmp}/hermes-kms-edid.XXXXXX")"; \
		cleanup() { $(RM) -r -- "$$test_tmp"; }; \
		trap cleanup EXIT; \
		trap 'exit 1' HUP INT TERM; \
		$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic \
			tests/edid.c -o "$$test_tmp/edid"; \
		"$$test_tmp/edid"; \
		printf 'synthetic EDID: PASS\n'

# Install the driver via DKMS so it persists across reboots and rebuilds for
# every new kernel (the same mechanism evdi-dkms uses). Run as root. Registered
# versions are removed before source directories so a DKMS failure leaves the
# matching /usr/src trees available for diagnosis and recovery.
dkms-install:
	@set -eu; \
		dkms_status="$$(dkms status)"; \
		versions="$$(printf '%s\n' "$$dkms_status" | \
			awk -F '[/,:]' '$$1 == "$(DKMS_NAME)" && $$2 != "" && !seen[$$2]++ { print $$2 }')"; \
		for version in $$versions; do \
			dkms remove -m '$(DKMS_NAME)' -v "$$version" --all; \
		done; \
		for src in /usr/src/$(DKMS_NAME)-*; do \
			[ -d "$$src" ] || continue; \
			$(RM) -r -- "$$src"; \
		done
	install -dm755 '$(DKMS_SRC)'
	cp -a Makefile dkms.conf include kernel '$(DKMS_SRC)/'
	@# Keep the source template release-independent; DKMS validates the installed
	@# copy against the version passed to add/build/install.
	@test "$$(grep -c '^PACKAGE_VERSION=' '$(DKMS_SRC)/dkms.conf')" -eq 1
	sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="$(DKMS_VERSION)"/' '$(DKMS_SRC)/dkms.conf'
	@grep -Fqx 'PACKAGE_VERSION="$(DKMS_VERSION)"' '$(DKMS_SRC)/dkms.conf'
	dkms add -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)'
	dkms build -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)'
	dkms install -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)'
	$(MAKE) install-runtime-udev
	$(MAKE) install-uapi DESTDIR=
	@printf 'Hermes-KMS installed via DKMS.\n'
	@if grep -q '^hermes_kms ' /proc/modules 2>/dev/null; then \
		printf 'An older Hermes-KMS module is already loaded; reboot once to activate version %s.\n' '$(DKMS_VERSION)'; \
	else \
		modprobe hermes_kms; \
	fi
	@printf 'To prepare the default independent-session pool for this user, run:\n'
	@printf '  sudo /usr/lib/hermes-kms/hermes-kms-setup configure --user auto\n'

dkms-uninstall:
	@set -eu; \
		dkms_status="$$(dkms status)"; \
		versions="$$(printf '%s\n' "$$dkms_status" | \
			awk -F '[/,:]' '$$1 == "$(DKMS_NAME)" && $$2 != "" && !seen[$$2]++ { print $$2 }')"; \
		for version in $$versions; do \
			dkms remove -m '$(DKMS_NAME)' -v "$$version" --all; \
		done; \
		for src in /usr/src/$(DKMS_NAME)-*; do \
			[ -d "$$src" ] || continue; \
			$(RM) -r -- "$$src"; \
		done
	$(MAKE) uninstall-runtime-udev
	$(MAKE) uninstall-uapi DESTDIR=
	@printf 'Hermes-KMS removed from DKMS.\n'

modules:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms $(LLVM_FLAG) modules

# Install a built module straight into a kernel's module tree, without DKMS.
#
# DKMS assumes a writable /usr and a kernel that can be rebuilt against on the
# running system. Image-based distributions (Bazzite, Silverblue, SteamOS and
# other bootc/ostree systems) have neither: /usr is read-only at runtime, and
# the module has to be baked in while the image is being built instead. There
# the module is compiled once against the image's kernel and dropped into
# /usr/lib/modules/$(KERNELRELEASE)/extra, which is what akmods and every
# ublue-os kmod image do.
#
#   make modules KERNELRELEASE=<kver> KDIR=/usr/lib/modules/<kver>/build
#   make modules-install KERNELRELEASE=<kver> DESTDIR=/
modules-install: MODULE_DEST := $(DESTDIR)/usr/lib/modules/$(KERNELRELEASE)/extra/hermes-kms
modules-install:
	install -Dm0644 kernel/hermes-kms/hermes_kms.ko '$(MODULE_DEST)/hermes_kms.ko'
	@# depmod resolves the tree through lib/modules, which is only the same as
	@# usr/lib/modules on a merged-/usr root. A bare staging DESTDIR has neither
	@# that symlink nor the rest of the kernel's module tree, so skip it there
	@# and say so, instead of failing a build that is otherwise complete.
	@if [ -d "$(DESTDIR)/lib/modules/$(KERNELRELEASE)" ]; then \
		depmod -b "$(DESTDIR)/" -a "$(KERNELRELEASE)"; \
	else \
		printf 'skipping depmod: no module tree at %s — run "depmod -a %s" on the target\n' \
			'$(DESTDIR)/lib/modules/$(KERNELRELEASE)' '$(KERNELRELEASE)' >&2; \
	fi
	@printf 'installed %s\n' '$(MODULE_DEST)/hermes_kms.ko'

# The module options, autoload entry, udev rules and seat helper, installed
# under /usr so they survive on an image-based system. Split out from
# install-runtime-udev because that target reloads udev and systemd, which
# cannot work inside an image build.
install-configs:
	install -Dm0644 packaging/modules-load.d/hermes-kms.conf \
		'$(DESTDIR)/usr/lib/modules-load.d/hermes-kms.conf'
	install -Dm0644 packaging/modprobe.d/hermes-kms.conf \
		'$(DESTDIR)/usr/lib/modprobe.d/hermes-kms.conf'
	install -Dm0644 udev/72-hermes-kms-session-seats.rules \
		'$(DESTDIR)$(SYSTEM_UDEV_RULE_DIR)/72-hermes-kms-session-seats.rules'
	install -Dm0644 udev/92-hermes-kms-access.rules \
		'$(DESTDIR)$(SYSTEM_UDEV_RULE_DIR)/92-hermes-kms-access.rules'
	install -Dm0755 scripts/hermes-kms-seatd-instance \
		'$(DESTDIR)/usr/lib/hermes-kms/hermes-kms-seatd-instance'
	install -Dm0755 scripts/hermes-kms-setup \
		'$(DESTDIR)/usr/lib/hermes-kms/hermes-kms-setup'
	install -Dm0644 packaging/systemd/hermes-kms-seatd@.service \
		'$(DESTDIR)/usr/lib/systemd/system/hermes-kms-seatd@.service'
	install -Dm0644 packaging/polkit/io.github.mroz59.hermes-kms.policy \
		'$(DESTDIR)/usr/share/polkit-1/actions/io.github.mroz59.hermes-kms.policy'

# Public, application-neutral headers for external controllers and capture
# consumers. The MIT helper is optional; the DRM UAPI remains usable directly.
install-uapi:
	install -Dm0644 include/uapi/drm/hermes_kms_drm.h \
		'$(DESTDIR)$(UAPI_INCLUDE_DIR)/hermes_kms_drm.h'
	install -Dm0644 tools/hermes_session.h \
		'$(DESTDIR)$(HERMES_INCLUDE_DIR)/hermes_session.h'
	install -Dm0644 LICENSES/MIT.txt \
		'$(DESTDIR)$(HERMES_LICENSE_DIR)/MIT.txt'

uninstall-uapi:
	$(RM) '$(DESTDIR)$(UAPI_INCLUDE_DIR)/hermes_kms_drm.h'
	$(RM) '$(DESTDIR)$(HERMES_INCLUDE_DIR)/hermes_session.h'
	$(RM) '$(DESTDIR)$(HERMES_LICENSE_DIR)/MIT.txt'
	-rmdir '$(DESTDIR)$(HERMES_INCLUDE_DIR)'
	-rmdir '$(DESTDIR)$(HERMES_LICENSE_DIR)'

tools: tools/hermes-kmsctl/hermes-kmsctl tools/hermes-kms-import-check/hermes-kms-import-check \
	tools/hermes-egl-import-check/hermes-egl-import-check tools/hermes-egl-import-check/pitch-detect \
	tools/hermes-sysmem-import-check/hermes-sysmem-import-check \
	tools/hermes-imported-scanout-test/hermes-imported-scanout-test \
	tools/hermes-pixel-peek/hermes_pixel_peek tools/hermes-cursor-probe/hermes_cursor_probe

tools/hermes-kmsctl/hermes-kmsctl: tools/hermes-kmsctl/hermes_kmsctl.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

tools/hermes-kms-import-check/hermes-kms-import-check: tools/hermes-kms-import-check/hermes_kms_import_check.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	@test -n "$(IMPORT_CHECK_LIBS)" || { printf 'missing libva/libva-drm/libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(IMPORT_CHECK_CFLAGS) -o $@ $< $(IMPORT_CHECK_LIBS)

tools/hermes-egl-import-check/hermes-egl-import-check: tools/hermes-egl-import-check/hermes_egl_import_check.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(EGL_CHECK_CFLAGS) $(EGL_CHECK_CUDA_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS) $(EGL_CHECK_CUDA_LIBS)

tools/hermes-imported-scanout-test/hermes-imported-scanout-test: tools/hermes-imported-scanout-test/hermes_imported_scanout_test.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	@test -n "$(CURSOR_PROBE_LIBS)" || { printf 'missing libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) -o $@ $< $(CURSOR_PROBE_LIBS)

tools/hermes-sysmem-import-check/hermes-sysmem-import-check: tools/hermes-sysmem-import-check/hermes_sysmem_import_check.c
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(EGL_CHECK_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS)

tools/hermes-egl-import-check/pitch-detect: tools/hermes-egl-import-check/pitch_detect.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(EGL_CHECK_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS)
tools/hermes-pixel-peek/hermes_pixel_peek: tools/hermes-pixel-peek/hermes_pixel_peek.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

tools/hermes-cursor-probe/hermes_cursor_probe: tools/hermes-cursor-probe/hermes_cursor_probe.c tools/hermes_session.h include/uapi/drm/hermes_kms_drm.h
	@test -n "$(CURSOR_PROBE_LIBS)" || { printf 'missing libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(CURSOR_PROBE_CFLAGS) -o $@ $< $(CURSOR_PROBE_LIBS)

install-runtime-udev:
	$(MAKE) install-configs DESTDIR=
	@# The rule moved from 70- to 72- so systemd's 70-uaccess.rules cannot
	@# re-add the tag it removes; drop the stale copy from older installs.
	$(RM) $(SYSTEM_UDEV_RULE_DIR)/70-hermes-kms-session-seats.rules
	-systemctl daemon-reload
	-udevadm control --reload-rules
	-udevadm trigger --subsystem-match=drm --action=change

uninstall-runtime-udev:
	-systemctl stop 'hermes-kms-seatd@*.service'
	@if [ -f /etc/udev/rules.d/90-hermes-kms-user.rules ] && \
		grep -qx '# Managed by hermes-kms-setup. Grants one configured UID render-node access.' \
			/etc/udev/rules.d/90-hermes-kms-user.rules; then \
		$(RM) /etc/udev/rules.d/90-hermes-kms-user.rules; \
	fi
	@if [ -f /etc/modprobe.d/hermes-kms.conf ] && \
		grep -qx '# Managed by hermes-kms-setup. Connectors stay disabled until owned.' \
			/etc/modprobe.d/hermes-kms.conf; then \
		$(RM) /etc/modprobe.d/hermes-kms.conf; \
	fi
	@if [ -f /etc/modules-load.d/hermes-kms.conf ] && \
		[ "$$(cat /etc/modules-load.d/hermes-kms.conf)" = hermes_kms ]; then \
		$(RM) /etc/modules-load.d/hermes-kms.conf; \
	fi
	@if [ -f /etc/hermes-kms/session-user ] && \
		grep -Eq '^[0-9]+$$' /etc/hermes-kms/session-user && \
		[ "$$(wc -l </etc/hermes-kms/session-user)" -eq 1 ]; then \
		$(RM) /etc/hermes-kms/session-user; \
	fi
	-rmdir /etc/hermes-kms
	$(RM) -r -- /run/hermes-kms-seatd
	$(RM) /usr/lib/modules-load.d/hermes-kms.conf
	$(RM) /usr/lib/modprobe.d/hermes-kms.conf
	$(RM) $(SYSTEM_UDEV_RULE_DIR)/72-hermes-kms-session-seats.rules
	$(RM) $(SYSTEM_UDEV_RULE_DIR)/92-hermes-kms-access.rules
	$(RM) $(SYSTEM_UDEV_RULE_DIR)/70-hermes-kms-session-seats.rules
	$(RM) /usr/lib/systemd/system/hermes-kms-seatd@.service
	$(RM) /usr/lib/hermes-kms/hermes-kms-seatd-instance
	$(RM) /usr/lib/hermes-kms/hermes-kms-setup
	$(RM) /usr/share/polkit-1/actions/io.github.mroz59.hermes-kms.policy
	-systemctl daemon-reload
	-udevadm control --reload-rules
	-udevadm trigger --subsystem-match=drm --action=change

install-dev-udev:
	install -m 0644 udev/99-hermes-kms-ignore-seat.rules $(UDEV_RULE_DIR)/99-hermes-kms-ignore-seat.rules
	$(RM) $(UDEV_RULE_DIR)/70-hermes-kms-ignore-seat.rules
	udevadm control --reload-rules
	udevadm trigger --subsystem-match=drm --action=change || true
	@printf 'installed %s\n' '$(UDEV_RULE_DIR)/99-hermes-kms-ignore-seat.rules'
	@printf 'Hermes-KMS primary nodes use group video, mode 0660, and uaccess for non-root control.\n'
	@printf 'If an old hermes_kms module is still open, log out/in or close openers, then run sudo rmmod hermes_kms.\n'

uninstall-dev-udev:
	$(RM) $(UDEV_RULE_DIR)/70-hermes-kms-ignore-seat.rules
	$(RM) $(UDEV_RULE_DIR)/99-hermes-kms-ignore-seat.rules
	udevadm control --reload-rules
	udevadm trigger --subsystem-match=drm --action=change || true
	@printf 'removed Hermes-KMS development udev rules\n'

clean-tools:
	$(RM) tools/hermes-kmsctl/hermes-kmsctl
	$(RM) tools/hermes-kms-import-check/hermes-kms-import-check
	$(RM) tools/hermes-egl-import-check/hermes-egl-import-check
	$(RM) tools/hermes-egl-import-check/pitch-detect
	$(RM) tools/hermes-sysmem-import-check/hermes-sysmem-import-check
	$(RM) tools/hermes-imported-scanout-test/hermes-imported-scanout-test
	$(RM) tools/hermes-pixel-peek/hermes_pixel_peek
	$(RM) tools/hermes-cursor-probe/hermes_cursor_probe

clean: clean-tools
	@if [ -f '$(KDIR)/Makefile' ]; then \
		$(MAKE) -C '$(KDIR)' M='$(PWD)/kernel/hermes-kms' $(LLVM_FLAG) clean; \
	else \
		$(RM) kernel/hermes-kms/*.o kernel/hermes-kms/*.ko \
			kernel/hermes-kms/*.mod kernel/hermes-kms/*.mod.c \
			kernel/hermes-kms/Module.symvers kernel/hermes-kms/modules.order \
			kernel/hermes-kms/.*.cmd; \
		$(RM) -r -- kernel/hermes-kms/.tmp_versions; \
		printf 'kernel build tree unavailable; removed local module artifacts directly\n'; \
	fi
