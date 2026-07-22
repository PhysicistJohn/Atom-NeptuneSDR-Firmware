# SPDX-License-Identifier: GPL-2.0-or-later OR MIT
ifneq ($(KERNELRELEASE),)
obj-$(CONFIG_NEPTUNE_STREAM) += neptune_stream.o
ccflags-y += -I$(src)/../../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules clean
all: modules

modules:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" CONFIG_NEPTUNE_STREAM=m modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
endif
