// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/errno.h>

#include "core.h"
#include "type_info.h"

static __nocfi unsigned long kr_name_to_addr(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return kallsyms_name_to_addr(name);
}

static struct ti_resolver kr_res = {
	.name_to_addr = kr_name_to_addr,
};

void ti_verify_ti(void);

int ti_cur_pid __used = 1;
int ti_ref_pid __used = 2;
extern int ti_mod_anchor;
extern int ti_btf_mode;
module_param(ti_cur_pid, int, 0);
module_param(ti_ref_pid, int, 0);
module_param(ti_mod_anchor, int, 0);
module_param(ti_btf_mode, int, 0);

static int __init type_info_init(void)
{
	int ret;

	find_kallsyms_base();
	if (!klnum_val) {
		pr_err("[type_info] kallsyms discovery failed\n");
		return -ENODEV;
	}

	ret = ti_init(&kr_res);
	if (ret) {
		pr_err("[type_info] init failed: %d\n", ret);
		return 0;
	}

	if (ti_btf_available())
		pr_info("[type_info] vmlinux btf ready, %u types\n",
			ti_type_count());
	else
		pr_warn("[type_info] vmlinux btf unavailable, degraded\n");
	ti_verify_ti();
	return 0;
}

static void __exit type_info_exit(void)
{
	ti_exit();
	pr_info("[type_info] exit\n");
}

module_init(type_info_init);
module_exit(type_info_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("type_info: runtime kernel struct layout via BTF");
