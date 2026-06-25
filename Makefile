KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
PWD := $(shell pwd)
LLVM ?= 1

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel/hermes-kms LLVM=$(LLVM) clean
