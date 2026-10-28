/* SPDX-License-Identifier: GPL-2.0 */
/*
 * macvlan_shim.h - 把内核未导出的 7 个符号重定向到本模块的 shim。
 *
 * 背景: 本机内核 CONFIG_MACVLAN=n + CONFIG_TRIM_UNUSED_KSYMS=y，这 7 个函数
 *       虽然存在（kallsyms 可见）但没有导出条目，标准模块链接时会报
 *       "Unknown symbol"。这里用宏把调用/取地址改名为 shim，shim 再用加载时
 *       传入的运行时地址调用真实函数（不受导出表与 KASLR 影响）。
 *
 * 由 -include 强制插到 macvlan_core.c 所有 include 之前，因此内核头文件里的
 * extern 声明也会一并改名，签名由编译器校验。
 */
#ifndef MACVLAN_SHIM_H
#define MACVLAN_SHIM_H

#include <linux/types.h>

/* -include 时机太早，内核头还没进来，先给指针类型做前置声明 */
struct net_device;
struct sk_buff;
struct net;
struct netlink_callback;
struct neighbour;
struct hh_cache;

int droid_mv_dev_forward_skb(struct net_device *dev, struct sk_buff *skb);
int droid_mv___dev_forward_skb(struct net_device *dev, struct sk_buff *skb);
int droid_mv_dev_mc_add_excl(struct net_device *dev, const unsigned char *addr);
struct sk_buff *droid_mv_ip_check_defrag(struct net *net, struct sk_buff *skb,
					 u32 user);
int droid_mv_ndo_dflt_fdb_dump(struct sk_buff *skb,
			       struct netlink_callback *cb,
			       struct net_device *dev,
			       struct net_device *filter_dev, int *idx);
int droid_mv_eth_header_cache(const struct neighbour *neigh,
			      struct hh_cache *hh, __be16 type);
void droid_mv_eth_header_cache_update(struct hh_cache *hh,
				      const struct net_device *dev,
				      const unsigned char *haddr);

#define dev_forward_skb			droid_mv_dev_forward_skb
#define __dev_forward_skb		droid_mv___dev_forward_skb
#define dev_mc_add_excl			droid_mv_dev_mc_add_excl
#define ip_check_defrag			droid_mv_ip_check_defrag
#define ndo_dflt_fdb_dump		droid_mv_ndo_dflt_fdb_dump
#define eth_header_cache		droid_mv_eth_header_cache
#define eth_header_cache_update		droid_mv_eth_header_cache_update

#endif /* MACVLAN_SHIM_H */
