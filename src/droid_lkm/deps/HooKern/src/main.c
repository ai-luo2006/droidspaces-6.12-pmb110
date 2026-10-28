// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>

#include "hk.h"
#include "hk_ksym.h"
#include "hk_patch.h"
#include "hk_ptr.h"
#include "hk_kprobe.h"
#include "hk_kretprobe.h"

extern unsigned long (*kallrecon_klp)(const char *name);
extern void find_kallsyms_base(void);

static unsigned long __nocfi demo_resolve(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return 0;
}

static unsigned long demo_sct;
static unsigned long demo_ni;

static const struct hk_sym demo_syms[] = {
	{ "sys_call_table", NULL, (void **)&demo_sct, true },
	{ "__arm64_sys_ni_syscall", NULL, (void **)&demo_ni, false },
	{ NULL, NULL, NULL, false },
};

static int demo_target(int v)
{
	return v * 2;
}

static int (*demo_fn)(int) = demo_target;
static int (*demo_orig_fn)(int);

static int demo_replacement(int v)
{
	return demo_orig_fn(v) + 1;
}

static bool demo_kprobe_hit;
static bool demo_kretprobe_hit;

static int demo_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (!demo_kprobe_hit) {
		demo_kprobe_hit = true;
		pr_info("[lkmhook] kprobe hit\n");
	}
	return 0;
}

static int demo_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (!demo_kretprobe_hit) {
		demo_kretprobe_hit = true;
		pr_info("[lkmhook] kretprobe hit\n");
	}
	return 0;
}

static struct hk_kprobe demo_kp;
static struct hk_kretprobe demo_krp;

static int __init lkmhook_init(void)
{
	struct hk_cfg cfg = {
		.resolve = demo_resolve,
	};
	int ret;

	find_kallsyms_base();
	if (!demo_resolve("sys_call_table")) {
		pr_warn("[lkmhook] kallsyms recovery failed\n");
		return -ENODATA;
	}

	ret = hk_init(&cfg);
	if (ret)
		return ret;

	ret = hk_ksym_register(demo_syms);
	if (ret)
		pr_warn("[lkmhook] sym register failed %d\n", ret);

	ret = hk_ptr_hook((void **)&demo_fn, demo_replacement,
			  (void **)&demo_orig_fn);
	if (ret) {
		pr_warn("[lkmhook] ptr hook failed %d\n", ret);
		return ret;
	}
	pr_info("[lkmhook] ptr chain %d\n", demo_fn(21));

	ret = hk_ptr_hook((void **)&demo_fn, demo_replacement, NULL);
	if (ret == -EEXIST)
		pr_info("[lkmhook] ptr dup rejected\n");

	ret = hk_ptr_hook(NULL, demo_replacement, NULL);
	if (ret == -EINVAL)
		pr_info("[lkmhook] ptr NULL rejected\n");

	ret = hk_patch_write((void *)0xdead000000000000UL, 0);
	if (ret == -EIO)
		pr_info("[lkmhook] patch bad addr rejected\n");

	ret = hk_kprobe_install(&demo_kp, "do_sys_openat2", demo_pre);
	if (ret)
		pr_warn("[lkmhook] kprobe failed %d\n", ret);

	ret = hk_kretprobe_install(&demo_krp, "do_sys_openat2", demo_ret);
	if (ret)
		pr_warn("[lkmhook] kretprobe failed %d\n", ret);

	pr_info("[lkmhook] loaded\n");
	return 0;
}

static void __exit lkmhook_exit(void)
{
	hk_kretprobe_remove(&demo_krp);
	hk_kprobe_remove(&demo_kp);
	hk_exit();
	pr_info("[lkmhook] ptr restored by exit %d\n", demo_fn(21));

	/*
	 * a hook whose restore failed still has its detour in kernel text, so the
	 * module must not unmap the trampoline it points at. the gate returns only
	 * once everything is back
	 */
	hk_exit_block();
	pr_info("[lkmhook] unloaded\n");
}

module_init(lkmhook_init);
module_exit(lkmhook_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LKMhook: kernel hook library demo");
