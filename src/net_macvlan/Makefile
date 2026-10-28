obj-m := macvlan.o
macvlan-y := macvlan_core.o macvlan_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

# -include : 7 个未导出符号重定向
# -DCONFIG_MACVLAN=1 : 让 if_macvlan.h 走真实实现，而不是 BUG() 存根
CFLAGS_macvlan_core.o := -include $(src)/macvlan_shim.h -DCONFIG_MACVLAN=1
CFLAGS_macvlan_shim.o := -DCONFIG_MACVLAN=1

# KallRecon: 从内核镜像里恢复 kallsyms 表，等价于内核自己的 kallsyms_lookup_name
ccflags-y += -Wno-declaration-after-statement -Wno-unused-variable \
	-Wno-unused-function -Wno-strict-prototypes
ccflags-y += -I$(src)/kr
