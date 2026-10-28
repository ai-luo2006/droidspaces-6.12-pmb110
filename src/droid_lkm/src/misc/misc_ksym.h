// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef DROID_LKM_MISC_KSYM_H
#define DROID_LKM_MISC_KSYM_H

#include <linux/types.h>
#include <linux/netfilter/x_tables.h>

struct net;
struct net_device;
struct dst_entry;
struct flowi;
struct in6_addr;
struct cred;
struct key;
struct mnt_idmap;
struct nsproxy;
struct fs_struct;
struct task_struct;
struct droid_lkm_ns_kind_reg;

/*
 * CONFIG_TRIM_UNUSED_KSYMS and the vendor abi_symbollist mean an exported symbol is not
 * necessarily in the ksymtab. everything is resolved by name, a missing thunk disables the
 * feature that needs it and never becomes an unresolved import
 */
struct droid_lkm_misc_ksym {
	/* xt */
	int (*xt_register_matches)(struct xt_match *match, unsigned int n);
	void (*xt_unregister_matches)(struct xt_match *match, unsigned int n);
	unsigned int (*inet_dev_addr_type)(struct net *net,
					   const struct net_device *dev,
					   __be32 addr);
	bool (*ipv6_chk_addr)(struct net *net, const struct in6_addr *addr,
			      const struct net_device *dev, int strict);
	int (*__ipv6_addr_type)(const struct in6_addr *addr);
	int (*__nf_ip6_route)(struct net *net, struct dst_entry **dst,
			      struct flowi *fl, bool strict);

	/* fake namespaces */
	int (*proc_alloc_inum)(unsigned int *inum);
	void (*proc_free_inum)(unsigned int inum);
	struct cred *(*prepare_creds)(void);
	int (*commit_creds)(struct cred *new);
	void (*key_put)(struct key *key);

	/* optional main module entry points */
	int (*ns_kind_register)(const struct droid_lkm_ns_kind_reg *kind);
	void (*ns_kind_unregister)(const struct droid_lkm_ns_kind_reg *kind);

	/* the single capability decision function, resolved for the trace only */
	int (*cap_capable)(const struct cred *cred, struct user_namespace *targ_ns,
			   int cap, unsigned int opts);

	/* address of the kernel's "no idmap" sentinel, compared by pointer */
	const struct mnt_idmap *nop_mnt_idmap;

	/* namespace construction, called with the container's own user namespace */
	struct nsproxy *(*create_new_namespaces)(unsigned long flags,
						 struct task_struct *tsk,
						 struct user_namespace *user_ns,
						 struct fs_struct *new_fs);

	/* the initial task, read for the initial mount namespace pointer */
	struct task_struct *init_task;
};

extern struct droid_lkm_misc_ksym droid_lkm_misc_ks;

/* mirrors the kernel inline, which masks the raw type down to 16 bits */
static inline int droid_lkm_misc_ipv6_addr_type(const struct in6_addr *addr)
{
	return droid_lkm_misc_ks.__ipv6_addr_type(addr) & 0xffff;
}

int droid_lkm_misc_ksym_init(void);
void droid_lkm_misc_ksym_probe(void);

#endif
