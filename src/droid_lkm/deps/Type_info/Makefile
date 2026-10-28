obj-m := type_info.o
type_info-objs := lib/port.o lib/btf.o lib/query.o lib/reg.o lib/lib.o lib/anchor.o lib/dwarf.o lib/slide.o src/main.o src/verify.o deps/KallRecon/lib/core.o deps/KallRecon/lib/anchor.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/deps/KallRecon/lib

ifeq ($(TI_PUBLIC_ANCHOR),1)
ccflags-y += -DCONFIG_TI_PUBLIC_ANCHOR
endif

ifeq ($(TI_FEATURE),1)
ccflags-y += -DCONFIG_TI_FEATURE
endif

ifeq ($(TI_DWARF),1)
ccflags-y += -DCONFIG_TI_DWARF
endif

ifeq ($(TI_DWARF_EXPORT),1)
ccflags-y += -DCONFIG_TI_DWARF
ccflags-y += -DCONFIG_TI_DWARF_EXPORT
endif

ifneq ($(TI_REMAP),0)
ccflags-y += -DCONFIG_TI_REMAP
endif

ifeq ($(TI_FUNC),1)
ccflags-y += -DCONFIG_TI_FUNC
endif

# the modname setter is only worth exporting when a consumer loads this module
# separately and resolves the symbol at run time. every project that links the
# library in carries the function itself, so the default is not to export it
ifeq ($(TI_MODNAME),1)
ccflags-y += -DCONFIG_TI_MODNAME
endif

# the cooperative registration entry points only need to be visible to a consumer
# that loads this module separately. a project that links the library in calls the
# functions directly, so the default is not to export them
ifeq ($(TI_REG),1)
ccflags-y += -DCONFIG_TI_REG
endif

# the self test is called by the library itself, so its entry point is not part of
# the surface either unless someone outside wants to trigger it
ifeq ($(TI_VERIFY),1)
ccflags-y += -DCONFIG_TI_VERIFY
endif

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(VER)

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	mkdir -p $(ODIR)
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
