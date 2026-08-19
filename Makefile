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

# Only pass LLVM=1 to kbuild; for gcc kernels pass nothing (LLVM=0 is also
# accepted by modern kbuild, but an empty flag is the safest default).
ifeq ($(LLVM),1)
LLVM_FLAG := LLVM=1
else
LLVM_FLAG :=
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
UDEV_RULE_DIR ?= /etc/udev/rules.d
SYSTEM_UDEV_RULE_DIR ?= /usr/lib/udev/rules.d

DKMS_NAME := hermes-kms
DKMS_VERSION := $(shell awk '/^#define HERMES_KMS_DRIVER_MAJOR/{maj=$$3} /^#define HERMES_KMS_DRIVER_MINOR/{min=$$3} /^#define HERMES_KMS_DRIVER_PATCH/{pat=$$3} END{print maj"."min"."pat}' kernel/hermes-kms/hermes_kms.c)
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

.PHONY: all modules tools install-runtime-udev uninstall-runtime-udev \
	install-dev-udev uninstall-dev-udev \
	dkms-install dkms-uninstall modules-install install-configs clean

all: modules tools

# Install the driver via DKMS so it persists across reboots and rebuilds for
# every new kernel (the same mechanism evdi-dkms uses). Run as root.
dkms-install:
	install -dm755 $(DKMS_SRC)
	cp -a Makefile dkms.conf include kernel packaging tools udev scripts $(DKMS_SRC)/
	dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION)
	$(MAKE) install-runtime-udev
	@printf 'Hermes-KMS installed via DKMS. Load it with: sudo modprobe hermes_kms initial_enabled=0\n'
	@printf 'For two independent sessions also run: sudo systemctl enable --now hermes-kms-seatd@1.service hermes-kms-seatd@2.service\n'

dkms-uninstall:
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all
	$(RM) -r $(DKMS_SRC)
	$(MAKE) uninstall-runtime-udev
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
	install -Dm0644 kernel/hermes-kms/hermes_kms.ko $(MODULE_DEST)/hermes_kms.ko
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
		$(DESTDIR)/usr/lib/modules-load.d/hermes-kms.conf
	install -Dm0644 packaging/modprobe.d/hermes-kms.conf \
		$(DESTDIR)/usr/lib/modprobe.d/hermes-kms.conf
	install -Dm0644 udev/70-hermes-kms-session-seats.rules \
		$(DESTDIR)$(SYSTEM_UDEV_RULE_DIR)/70-hermes-kms-session-seats.rules
	install -Dm0755 scripts/hermes-kms-seatd-instance \
		$(DESTDIR)/usr/lib/hermes-kms/hermes-kms-seatd-instance
	install -Dm0644 packaging/systemd/hermes-kms-seatd@.service \
		$(DESTDIR)/usr/lib/systemd/system/hermes-kms-seatd@.service

tools: tools/hermes-kmsctl/hermes-kmsctl tools/hermes-kms-import-check/hermes-kms-import-check \
	tools/hermes-egl-import-check/hermes-egl-import-check tools/hermes-egl-import-check/pitch-detect \
	tools/hermes-sysmem-import-check/hermes-sysmem-import-check

tools/hermes-kmsctl/hermes-kmsctl: tools/hermes-kmsctl/hermes_kmsctl.c include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

tools/hermes-kms-import-check/hermes-kms-import-check: tools/hermes-kms-import-check/hermes_kms_import_check.c include/uapi/drm/hermes_kms_drm.h
	@test -n "$(IMPORT_CHECK_LIBS)" || { printf 'missing libva/libva-drm/libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(IMPORT_CHECK_CFLAGS) -o $@ $< $(IMPORT_CHECK_LIBS)

tools/hermes-egl-import-check/hermes-egl-import-check: tools/hermes-egl-import-check/hermes_egl_import_check.c include/uapi/drm/hermes_kms_drm.h
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(EGL_CHECK_CFLAGS) $(EGL_CHECK_CUDA_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS) $(EGL_CHECK_CUDA_LIBS)

tools/hermes-sysmem-import-check/hermes-sysmem-import-check: tools/hermes-sysmem-import-check/hermes_sysmem_import_check.c
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(EGL_CHECK_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS)

tools/hermes-egl-import-check/pitch-detect: tools/hermes-egl-import-check/pitch_detect.c include/uapi/drm/hermes_kms_drm.h
	@test -n "$(EGL_CHECK_LIBS)" || { printf 'missing libdrm/gbm/egl/gl pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(EGL_CHECK_CFLAGS) -o $@ $< $(EGL_CHECK_LIBS)

install-runtime-udev:
	install -Dm0644 udev/70-hermes-kms-session-seats.rules \
		$(SYSTEM_UDEV_RULE_DIR)/70-hermes-kms-session-seats.rules
	install -Dm0755 scripts/hermes-kms-seatd-instance \
		/usr/lib/hermes-kms/hermes-kms-seatd-instance
	install -Dm0644 packaging/systemd/hermes-kms-seatd@.service \
		/usr/lib/systemd/system/hermes-kms-seatd@.service
	-systemctl daemon-reload
	-udevadm control --reload-rules
	-udevadm trigger --subsystem-match=drm --action=change

uninstall-runtime-udev:
	$(RM) $(SYSTEM_UDEV_RULE_DIR)/70-hermes-kms-session-seats.rules
	$(RM) /usr/lib/systemd/system/hermes-kms-seatd@.service
	$(RM) /usr/lib/hermes-kms/hermes-kms-seatd-instance
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

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms $(LLVM_FLAG) clean
	$(RM) tools/hermes-kmsctl/hermes-kmsctl
	$(RM) tools/hermes-kms-import-check/hermes-kms-import-check
	$(RM) tools/hermes-egl-import-check/hermes-egl-import-check
	$(RM) tools/hermes-egl-import-check/pitch-detect
