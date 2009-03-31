obj-$(CONFIG_LAC) += lacp.o

lacp-objs := lac.o lac_device.o lac_if.o lac_ioctl.o lac_input.o lac_protocol.o lac_debug.o lac_util.o lac_notify.o 

CFGFLAGS += -DDEBUG -g

EXTRA_CFLAGS += $(CFGFLAGS)

# KDIR := /lib/modules/$(KVER)/build
KDIR := ../..

PWD=$(shell pwd)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -f *.o *.ko *.mod.* *.mod
