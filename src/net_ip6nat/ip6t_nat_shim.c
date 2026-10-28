// SPDX-License-Identifier: GPL-2.0
/*
 * ip6t_nat_shim.c - 用运行时地址调用内核未导出的 9 个函数。
 *
 * 地址由加载脚本从 /proc/kallsyms 读出后作为模块参数传入（KASLR 每次开机变化）。
 * 所有 wrapper 标 __nocfi，避免 KCFI 对间接调用做类型校验。
 * 地址为 0 时返回优雅失败值，不会跳转到 0。
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv6.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_nat.h>
#include "kr/core.h"

static unsigned long a_alloc_initial_table;
static unsigned long a_register_table;
static unsigned long a_unregister_table_exit;
static unsigned long a_do_table;
static unsigned long a_find_table;
static unsigned long a_register_template;
static unsigned long a_unregister_template;
static unsigned long a_nat_reg_fn;
static unsigned long a_nat_unreg_fn;

module_param_named(addr_ip6t_alloc_initial_table, a_alloc_initial_table, ulong, 0444);
module_param_named(addr_ip6t_register_table, a_register_table, ulong, 0444);
module_param_named(addr_ip6t_unregister_table_exit, a_unregister_table_exit, ulong, 0444);
module_param_named(addr_ip6t_do_table, a_do_table, ulong, 0444);
module_param_named(addr_xt_find_table, a_find_table, ulong, 0444);
module_param_named(addr_xt_register_template, a_register_template, ulong, 0444);
module_param_named(addr_xt_unregister_template, a_unregister_template, ulong, 0444);
module_param_named(addr_nf_nat_ipv6_register_fn, a_nat_reg_fn, ulong, 0444);
module_param_named(addr_nf_nat_ipv6_unregister_fn, a_nat_unreg_fn, ulong, 0444);

static bool nat_warned;

/*
 * 自解析: 参数没给地址时，模块自己在 init 里从内核 kallsyms 表查。
 * 不读 /proc/kallsyms（kptr_restrict=2 时内核态读也是 0），
 * 而是用 KallRecon 恢复内核自己的 kallsyms_lookup_name 再按名字查。
 */
int __init droid_nat_shim_resolve(void)
{
	static const struct {
		const char *name;
		unsigned long *slot;
		bool req;	/* 缺了就不能工作的符号 */
	} syms[] = {
		{ "ip6t_alloc_initial_table",  &a_alloc_initial_table, true  },
		{ "ip6t_register_table",       &a_register_table,      true  },
		{ "ip6t_do_table",             &a_do_table,            true  },
		{ "xt_register_template",      &a_register_template,   true  },
		{ "nf_nat_ipv6_register_fn",   &a_nat_reg_fn,          true  },
		{ "ip6t_unregister_table_exit",&a_unregister_table_exit, false },
		{ "xt_find_table",             &a_find_table,          false },
		{ "xt_unregister_template",    &a_unregister_template, false },
		{ "nf_nat_ipv6_unregister_fn", &a_nat_unreg_fn,        false },
	};
	int i, miss = 0, hard = 0;

	find_kallsyms_base();
	if (!klnum_val || !kallrecon_klp) {
		pr_err("[ip6t-nat-shim] kallsyms recovery failed\n");
		return -ENODATA;
	}

	for (i = 0; i < (int)ARRAY_SIZE(syms); i++) {
		if (!*syms[i].slot)
			*syms[i].slot = kallrecon_klp(syms[i].name);
		if (!*syms[i].slot) {
			pr_warn("[ip6t-nat-shim] unresolved: %s (%s)\n",
				syms[i].name, syms[i].req ? "required" : "optional");
			miss++;
			if (syms[i].req)
				hard++;
		}
	}

	pr_info("[ip6t-nat-shim] self-resolved %d/%zu symbols%s\n",
		(int)ARRAY_SIZE(syms) - miss, ARRAY_SIZE(syms),
		hard ? " (required missing)" : "");
	return hard ? -ENOENT : 0;
}

static void nat_warn_once(const char *n)
{
	if (!nat_warned) {
		nat_warned = true;
		pr_warn("[ip6t-nat-shim] missing runtime address for %s; "
			"pass addr_%s=<kallsyms addr>\n", n, n);
	}
}

void *__nocfi droid_nat_ip6t_alloc_initial_table(const struct xt_table *t)
{
	typeof(&ip6t_alloc_initial_table) fn = (void *)a_alloc_initial_table;

	if (unlikely(!fn)) {
		nat_warn_once("ip6t_alloc_initial_table");
		return NULL;
	}
	return fn(t);
}

int __nocfi droid_nat_ip6t_register_table(struct net *net,
					  const struct xt_table *table,
					  const struct ip6t_replace *repl,
					  const struct nf_hook_ops *ops)
{
	typeof(&ip6t_register_table) fn = (void *)a_register_table;

	if (unlikely(!fn)) {
		nat_warn_once("ip6t_register_table");
		return -ENOSYS;
	}
	return fn(net, table, repl, ops);
}

void __nocfi droid_nat_ip6t_unregister_table_exit(struct net *net,
						  const char *name)
{
	typeof(&ip6t_unregister_table_exit) fn = (void *)a_unregister_table_exit;

	if (unlikely(!fn)) {
		nat_warn_once("ip6t_unregister_table_exit");
		return;
	}
	fn(net, name);
}

unsigned int __nocfi droid_nat_ip6t_do_table(void *priv, struct sk_buff *skb,
					     const struct nf_hook_state *state)
{
	typeof(&ip6t_do_table) fn = (void *)a_do_table;

	if (unlikely(!fn)) {
		nat_warn_once("ip6t_do_table");
		return NF_ACCEPT;	/* 无法处理时放行，不丢包 */
	}
	return fn(priv, skb, state);
}

struct xt_table *__nocfi droid_nat_xt_find_table(struct net *net, u8 af,
						 const char *name)
{
	typeof(&xt_find_table) fn = (void *)a_find_table;

	if (unlikely(!fn)) {
		nat_warn_once("xt_find_table");
		return NULL;
	}
	return fn(net, af, name);
}

int __nocfi droid_nat_xt_register_template(const struct xt_table *t,
					   int (*table_init)(struct net *net))
{
	typeof(&xt_register_template) fn = (void *)a_register_template;

	if (unlikely(!fn)) {
		nat_warn_once("xt_register_template");
		return -ENOSYS;
	}
	return fn(t, table_init);
}

void __nocfi droid_nat_xt_unregister_template(const struct xt_table *t)
{
	typeof(&xt_unregister_template) fn = (void *)a_unregister_template;

	if (unlikely(!fn)) {
		nat_warn_once("xt_unregister_template");
		return;
	}
	fn(t);
}

int __nocfi droid_nat_nf_nat_ipv6_register_fn(struct net *net,
					      const struct nf_hook_ops *ops)
{
	typeof(&nf_nat_ipv6_register_fn) fn = (void *)a_nat_reg_fn;

	if (unlikely(!fn)) {
		nat_warn_once("nf_nat_ipv6_register_fn");
		return -ENOSYS;
	}
	return fn(net, ops);
}

void __nocfi droid_nat_nf_nat_ipv6_unregister_fn(struct net *net,
						 const struct nf_hook_ops *ops)
{
	typeof(&nf_nat_ipv6_unregister_fn) fn = (void *)a_nat_unreg_fn;

	if (unlikely(!fn)) {
		nat_warn_once("nf_nat_ipv6_unregister_fn");
		return;
	}
	fn(net, ops);
}
