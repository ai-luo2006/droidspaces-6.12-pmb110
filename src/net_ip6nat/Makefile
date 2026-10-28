obj-m := ip6table_nat.o
ip6table_nat-y := ip6table_nat_core.o ip6t_nat_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

# 9 个未导出符号重定向
CFLAGS_ip6table_nat_core.o := -include $(src)/ip6t_nat_shim.h

# KallRecon: 恢复内核 kallsyms 表，模块自解析地址
ccflags-y += -Wno-declaration-after-statement -Wno-unused-variable \
	-Wno-unused-function -Wno-strict-prototypes
ccflags-y += -I$(src)/kr
