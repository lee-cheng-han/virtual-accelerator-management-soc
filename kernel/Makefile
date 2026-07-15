ifneq ($(KERNELRELEASE),)

obj-m += vams_pci.o
ccflags-y += -I$(src)

ifeq ($(VAMS_TESTING),1)
ccflags-y += -DCONFIG_VAMS_PCI_TESTING
endif

else

KERNEL_BUILD ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all modules clean

all: modules

modules:
	@test -f "$(KERNEL_BUILD)/Makefile" || { \
		echo "kernel build tree not found: $(KERNEL_BUILD)" >&2; \
		exit 2; \
	}
	$(MAKE) -C "$(KERNEL_BUILD)" M="$(PWD)" \
		VAMS_TESTING="$(VAMS_TESTING)" W=1 modules

clean:
	@test ! -f "$(KERNEL_BUILD)/Makefile" || \
		$(MAKE) -C "$(KERNEL_BUILD)" M="$(PWD)" clean

endif
