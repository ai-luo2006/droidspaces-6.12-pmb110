// SPDX-License-Identifier: GPL-2.0
/*
 * macvlan_shim.c - 用运行时地址调用内核未导出的 7 个函数。
 *
 * 地址由加载脚本从 /proc/kallsyms 读出后作为模块参数传入（KASLR 每次开机变化，
 * 所以不能编死）。所有 wrapper 都标 __nocfi，避免 KCFI 对间接调用做类型校验。
 *
 * 地址为 0 时不崩溃，返回"优雅失败"（-ENOSYS / 原 skb / 0），只是功能不可用。
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <net/ip.h>
#include "kr/core.h"

static unsigned long mv_a_dev_forward_skb;
static unsigned long mv_a___dev_forward_skb;
static unsigned long mv_a_dev_mc_add_excl;
static unsigned long mv_a_ip_check_defrag;
static unsigned long mv_a_ndo_dflt_fdb_dump;
static unsigned long mv_a_eth_header_cache;
static unsigned long mv_a_eth_header_cache_update;

module_param_named(addr_dev_forward_skb, mv_a_dev_forward_skb, ulong, 0444);
MODULE_PARM_DESC(addr_dev_forward_skb, "runtime address of dev_forward_skb");
module_param_named(addr___dev_forward_skb, mv_a___dev_forward_skb, ulong, 0444);
MODULE_PARM_DESC(addr___dev_forward_skb, "runtime address of __dev_forward_skb");
module_param_named(addr_dev_mc_add_excl, mv_a_dev_mc_add_excl, ulong, 0444);
MODULE_PARM_DESC(addr_dev_mc_add_excl, "runtime address of dev_mc_add_excl");
module_param_named(addr_ip_check_defrag, mv_a_ip_check_defrag, ulong, 0444);
MODULE_PARM_DESC(addr_ip_check_defrag, "runtime address of ip_check_defrag");
module_param_named(addr_ndo_dflt_fdb_dump, mv_a_ndo_dflt_fdb_dump, ulong, 0444);
MODULE_PARM_DESC(addr_ndo_dflt_fdb_dump, "runtime address of ndo_dflt_fdb_dump");
module_param_named(addr_eth_header_cache, mv_a_eth_header_cache, ulong, 0444);
MODULE_PARM_DESC(addr_eth_header_cache, "runtime address of eth_header_cache");
module_param_named(addr_eth_header_cache_update, mv_a_eth_header_cache_update,
		   ulong, 0444);
MODULE_PARM_DESC(addr_eth_header_cache_update,
		 "runtime address of eth_header_cache_update");

static bool mv_warned;

/*
 * 自解析: 参数没给地址时，模块自己在 init 里从内核 kallsyms 表里查。
 * 不读 /proc/kallsyms(受 kptr_restrict=2 限制，内核态读同样是 0)，
 * 而是用 KallRecon 恢复内核自己的 kallsyms_lookup_name 再按名字查，
 * 因此加载命令里不再需要 addr_xxx= 参数，也不需要改 kptr_restrict。
 */
int __init droid_mv_shim_resolve(void)
{
	static const struct {
		const char *name;
		unsigned long *slot;
		bool req;	/* 缺了就不能工作的符号 */
	} syms[] = {
		{ "dev_forward_skb",        &mv_a_dev_forward_skb,        true  },
		{ "__dev_forward_skb",      &mv_a___dev_forward_skb,      true  },
		{ "dev_mc_add_excl",        &mv_a_dev_mc_add_excl,        true  },
		{ "eth_header_cache",       &mv_a_eth_header_cache,       true  },
		{ "ip_check_defrag",        &mv_a_ip_check_defrag,        false },
		{ "ndo_dflt_fdb_dump",      &mv_a_ndo_dflt_fdb_dump,      false },
		{ "eth_header_cache_update",&mv_a_eth_header_cache_update,false },
	};
	int i, miss = 0, hard = 0;

	find_kallsyms_base();
	if (!klnum_val || !kallrecon_klp) {
		pr_err("[macvlan-shim] kallsyms recovery failed\n");
		return -ENODATA;
	}

	for (i = 0; i < (int)ARRAY_SIZE(syms); i++) {
		if (!*syms[i].slot)
			*syms[i].slot = kallrecon_klp(syms[i].name);
		if (!*syms[i].slot) {
			pr_warn("[macvlan-shim] unresolved: %s (%s)\n",
				syms[i].name, syms[i].req ? "required" : "optional");
			miss++;
			if (syms[i].req)
				hard++;
		}
	}

	pr_info("[macvlan-shim] self-resolved %d/%zu symbols%s\n",
		(int)ARRAY_SIZE(syms) - miss, ARRAY_SIZE(syms),
		hard ? " (required missing)" : "");
	return hard ? -ENOENT : 0;
}

static void mv_warn_once(const char *name)
{
	if (!mv_warned) {
		mv_warned = true;
		pr_warn("[macvlan-shim] missing runtime address for %s; "
			"pass addr_%s=<kallsyms addr>\n", name, name);
	}
}

int __nocfi droid_mv_dev_forward_skb(struct net_device *dev,
				     struct sk_buff *skb)
{
	typeof(&dev_forward_skb) fn = (void *)mv_a_dev_forward_skb;

	if (unlikely(!fn)) {
		mv_warn_once("dev_forward_skb");
		return -ENOSYS;
	}
	return fn(dev, skb);
}

int __nocfi droid_mv___dev_forward_skb(struct net_device *dev,
				       struct sk_buff *skb)
{
	typeof(&__dev_forward_skb) fn = (void *)mv_a___dev_forward_skb;

	if (unlikely(!fn)) {
		mv_warn_once("__dev_forward_skb");
		return -ENOSYS;
	}
	return fn(dev, skb);
}

int __nocfi droid_mv_dev_mc_add_excl(struct net_device *dev,
				     const unsigned char *addr)
{
	typeof(&dev_mc_add_excl) fn = (void *)mv_a_dev_mc_add_excl;

	if (unlikely(!fn)) {
		mv_warn_once("dev_mc_add_excl");
		return -ENOSYS;
	}
	return fn(dev, addr);
}

struct sk_buff *__nocfi droid_mv_ip_check_defrag(struct net *net,
						 struct sk_buff *skb, u32 user)
{
	typeof(&ip_check_defrag) fn = (void *)mv_a_ip_check_defrag;

	if (unlikely(!fn)) {
		mv_warn_once("ip_check_defrag");
		return skb;	/* 不做分片检查，直接放行 */
	}
	return fn(net, skb, user);
}

int __nocfi droid_mv_ndo_dflt_fdb_dump(struct sk_buff *skb,
				       struct netlink_callback *cb,
				       struct net_device *dev,
				       struct net_device *filter_dev, int *idx)
{
	typeof(&ndo_dflt_fdb_dump) fn = (void *)mv_a_ndo_dflt_fdb_dump;

	if (unlikely(!fn)) {
		mv_warn_once("ndo_dflt_fdb_dump");
		return 0;
	}
	return fn(skb, cb, dev, filter_dev, idx);
}

int __nocfi droid_mv_eth_header_cache(const struct neighbour *neigh,
				      struct hh_cache *hh, __be16 type)
{
	typeof(&eth_header_cache) fn = (void *)mv_a_eth_header_cache;

	if (unlikely(!fn)) {
		mv_warn_once("eth_header_cache");
		return -EINVAL;
	}
	return fn(neigh, hh, type);
}

void __nocfi droid_mv_eth_header_cache_update(struct hh_cache *hh,
					      const struct net_device *dev,
					      const unsigned char *haddr)
{
	typeof(&eth_header_cache_update) fn = (void *)mv_a_eth_header_cache_update;

	if (unlikely(!fn)) {
		mv_warn_once("eth_header_cache_update");
		return;
	}
	fn(hh, dev, haddr);
}
