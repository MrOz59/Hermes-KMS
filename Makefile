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
UDEV_RULE_DIR ?= /etc/udev/rules.d
SYSTEM_UDEV_RULE_DIR ?= /usr/lib/udev/rules.d

DKMS_NAME := hermes-kms
DKMS_VERSION := $(shell awk '/^#define HERMES_KMS_DRIVER_MAJOR/{maj=$$3} /^#define HERMES_KMS_DRIVER_MINOR/{min=$$3} /^#define HERMES_KMS_DRIVER_PATCH/{pat=$$3} END{print maj"."min"."pat}' kernel/hermes-kms/hermes_kms.c)
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

.PHONY: all modules tools install-runtime-udev uninstall-runtime-udev \
	install-dev-udev uninstall-dev-udev \
	dkms-install dkms-uninstall clean

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

tools: tools/hermes-kmsctl/hermes-kmsctl tools/hermes-kms-import-check/hermes-kms-import-check

tools/hermes-kmsctl/hermes-kmsctl: tools/hermes-kmsctl/hermes_kmsctl.c include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

tools/hermes-kms-import-check/hermes-kms-import-check: tools/hermes-kms-import-check/hermes_kms_import_check.c include/uapi/drm/hermes_kms_drm.h
	@test -n "$(IMPORT_CHECK_LIBS)" || { printf 'missing libva/libva-drm/libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(IMPORT_CHECK_CFLAGS) -o $@ $< $(IMPORT_CHECK_LIBS)

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
