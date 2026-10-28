obj-m := lkmhook.o

lkmhook-y := src/main.o lib/hk.o lib/hk_ksym.o lib/hk_patch.o lib/hk_flush.o lib/hk_ptr.o \
	lib/hk_inline.o lib/hk_kprobe.o lib/hk_redir.o lib/hk_kretprobe.o lib/hk_sighook.o \
	lib/hk_cfi.o lib/hk_binder.o \
	deps/KallRecon/lib/core.o deps/KallRecon/lib/slide.o deps/KallRecon/lib/anchor.o

ifeq ($(HK_LSM),1)
lkmhook-y += lib/hk_lsm.o
ccflags-y += -DCONFIG_HK_LSM
endif

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/deps/KallRecon/lib

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
