CONFIG_NETMAP:=m
CONFIG_IXGBE:=m
CONFIG_IGB:=m

netmap_lin-objs := netmap.o

obj-$(CONFIG_NETMAP) = netmap_lin.o
# obj-m += $(DRIVERS)
obj-$(CONFIG_IXGBE) += ixgbe/

# names of the driver sources. In old linuxes are under
# KSRC/drivers/net, but depending on where you build they
# can be in $(KSRC)/source/drivers/net/ethernet/$(manufacturer)
DRIVER_SRCS =  ixgbe/ igb/
DRIVER_SUBDIRS= intel

# The following commands are needed to build the modules as out-of-tree, in
# fact the kernel sources path must be specified.

# Additional compile flags (e.g. header location)
M:=$(PWD)
EXTRA_CFLAGS := -I$(M) -I$(M)/../sys -I$(M)/../sys/dev -DCONFIG_NETMAP

# We use KSRC for the kernel configuration and sources.
# If the sources are elsewhere, then use SRC to point to them.
KSRC ?= /lib/modules/$(shell uname -r)/build
SRC ?= $(KSRC)

# extract version number and filter with the available patches.
LIN_VER = $(shell grep LINUX_VERSION_CODE $(KSRC)/include/linux/version.h | \
	awk '{printf "%03x%02d", $$3/256, $$3%256} ')


# all:	get-drivers build
all:	build

build:
	make -C $(KSRC) M=$(PWD) CONFIG_NETMAP=m	\
		CONFIG_IXGBE=m CONFIG_IGB=m EXTRA_CFLAGS='$(EXTRA_CFLAGS)' \
		DRIVERS="ixgb/ ixgbe/" modules
	@ls -l `find . -name \*.ko`

clean:
	-@ make -C $(KSRC) M=$(PWD) clean
	-@ (rm -rf *.orig *.rej *.ko *.o )

# the source is not here so we need to specify a dependency
$(obj)/netmap.o: $(M)/../sys/dev/netmap/netmap.c
	$(call cmd,cc_o_c)
	$(call cmd,modversions)
#	$(call quiet_cmd,cc_o_c)

apps:
	(cd ../examples; make)
