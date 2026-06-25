KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
PWD := $(shell pwd)
LLVM ?= 1
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
UAPI_CFLAGS := -I$(PWD)/include/uapi

.PHONY: all modules clean

all: modules tools

modules:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) modules

tools: tools/hermes-kmsctl/hermes-kmsctl

tools/hermes-kmsctl/hermes-kmsctl: tools/hermes-kmsctl/hermes_kmsctl.c include/uapi/drm/hermes_kms_drm.h
	$(CC) $(CFLAGS) $(UAPI_CFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) clean
	$(RM) tools/hermes-kmsctl/hermes-kmsctl
