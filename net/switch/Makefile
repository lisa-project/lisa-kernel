obj-$(CONFIG_SWITCH) += switch.o

switch-objs := sw.o sw_fdb.o sw_proc.o sw_vdb.o sw_forward.o sw_ioctl.o sw_vif.o sw_socket.o

##
# Config section; will be moved to kernel config
##

# What to do when a non-trunk interface is assigned a vlan that does
# not exist:
# 1			Create the requested vlan (if we're not vtp client)
# 2			Shutdown the interface
CFGFLAGS += -DNET_SWITCH_NOVLANFORIF=1

# Force default vlans to always be allowed in trunk mode
# CFGFLAGS += -DNET_SWITCH_TRUNKDEFAULTVLANS

# Make additional checks when identifying CDP, VTP and STP frames
CFGFLAGS += -DSW_SOCK_FILTER_PARANOIA

# Enable debugging
CFGFLAGS += -DDEBUG -g

##
# End of config section
##

EXTRA_CFLAGS += $(CFGFLAGS)

# KDIR := /lib/modules/$(KVER)/build
KDIR := ../..

PWD=$(shell pwd)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	rm -f *.o *.ko *.mod.* *.mod
