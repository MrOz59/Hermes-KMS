KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
PWD := $(shell pwd)
LLVM ?= 1
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
UAPI_CFLAGS := -I$(PWD)/include/uapi
IMPORT_CHECK_CFLAGS := $(shell pkg-config --cflags libva libva-drm libdrm 2>/dev/null)
IMPORT_CHECK_LIBS := $(shell pkg-config --libs libva libva-drm libdrm 2>/dev/null)
UDEV_RULE_DIR ?= /etc/udev/rules.d

.PHONY: all modules tools install-dev-udev uninstall-dev-udev clean

all: modules tools

modules:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) modules

tools: tools/hermes-kmsctl/hermes-kmsctl tools/hermes-kms-import-check/hermes-kms-import-check

tools/hermes-kmsctl/hermes-kmsctl: tools/hermes-kmsctl/hermes_kmsctl.c include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

tools/hermes-kms-import-check/hermes-kms-import-check: tools/hermes-kms-import-check/hermes_kms_import_check.c include/uapi/drm/hermes_kms_drm.h
	@test -n "$(IMPORT_CHECK_LIBS)" || { printf 'missing libva/libva-drm/libdrm pkg-config metadata\n' >&2; exit 1; }
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) $(IMPORT_CHECK_CFLAGS) -o $@ $< $(IMPORT_CHECK_LIBS)

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
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) clean
	$(RM) tools/hermes-kmsctl/hermes-kmsctl
	$(RM) tools/hermes-kms-import-check/hermes-kms-import-check
