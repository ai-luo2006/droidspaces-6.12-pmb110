obj-m := kerncall.o

kerncall-y := src/main.o lib/sc.o lib/sc_slide.o \
	deps/KallRecon/lib/core.o deps/KallRecon/lib/slide.o deps/KallRecon/lib/anchor.o \
	deps/HooKern/lib/hk.o deps/HooKern/lib/hk_ksym.o \
	deps/HooKern/lib/hk_patch.o deps/HooKern/lib/hk_flush.o \
	deps/HooKern/lib/hk_ptr.o deps/HooKern/lib/hk_inline.o \
	deps/HooKern/lib/hk_kprobe.o deps/HooKern/lib/hk_redir.o \
	deps/HooKern/lib/hk_kretprobe.o \
	deps/HooKern/lib/hk_sighook.o deps/HooKern/lib/hk_cfi.o \
	deps/HooKern/lib/hk_binder.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/deps/KallRecon/lib
ccflags-y += -I$(src)/deps/HooKern/lib

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

ifneq ($(KERNSC_MINIMAL),1)
kerncall-y += lib/sc_sock.o
ccflags-y += -DCONFIG_KERNSC_DISCOVER -DCONFIG_KERNSC_PATCH -DCONFIG_KERNSC_TP -DCONFIG_KERNSC_REDIR -DCONFIG_KERNSC_SOCK
endif

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
