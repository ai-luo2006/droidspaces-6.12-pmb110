/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ip6t_nat_shim.h - 把内核未导出的 9 个符号重定向到本模块的 shim。
 *
 * 本机内核 CONFIG_IP6_NF_NAT=n + CONFIG_TRIM_UNUSED_KSYMS=y，这些函数存在
 * （kallsyms 可见）但没有导出条目。这里用宏把调用/取地址改名为 shim，shim 再用
 * 加载时传入的运行时地址调用真实函数。
 *
 * 由 -include 强制插到 ip6table_nat.c 所有 include 之前，内核头文件里的 extern
 * 声明会一并改名，签名由编译器校验（因此本文件不需要再写前置声明）。
 */
#ifndef IP6T_NAT_SHIM_H
#define IP6T_NAT_SHIM_H

#define ip6t_alloc_initial_table	droid_nat_ip6t_alloc_initial_table
#define ip6t_register_table		droid_nat_ip6t_register_table
#define ip6t_unregister_table_exit	droid_nat_ip6t_unregister_table_exit
#define ip6t_do_table			droid_nat_ip6t_do_table
#define xt_find_table			droid_nat_xt_find_table
#define xt_register_template		droid_nat_xt_register_template
#define xt_unregister_template		droid_nat_xt_unregister_template
#define nf_nat_ipv6_register_fn		droid_nat_nf_nat_ipv6_register_fn
#define nf_nat_ipv6_unregister_fn	droid_nat_nf_nat_ipv6_unregister_fn

#endif /* IP6T_NAT_SHIM_H */
