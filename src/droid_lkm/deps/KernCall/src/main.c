// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <asm/page.h>
#include <asm/unistd.h>

#include "sc.h"
#include "sc_sock.h"

extern unsigned long (*kallrecon_klp)(const char *name);
extern void find_kallsyms_base(void);

static char sc_key[SC_KEY_MAX] = "kerncall";
module_param_string(key, sc_key, sizeof(sc_key), 0400);

static int find_slot_mode;
module_param(find_slot_mode, int, 0);

static int tp_enable = 1;
module_param(tp_enable, int, 0);

static int tp_mark_all = 1;
module_param(tp_mark_all, int, 0);

static int tp_intercept;
module_param(tp_intercept, int, 0);

static int no_patch;
module_param(no_patch, int, 0);

static int tp_hook_init;
module_param(tp_hook_init, int, 0);

static unsigned long __nocfi kr_name_to_addr(const char *name)
{
	if (kallrecon_klp)
		return kallrecon_klp(name);
	return 0;
}

static bool is_task_size_cand(unsigned long v)
{
	if (v == (1UL << 39) || v == (1UL << 48) || v == (1UL << 52))
		return true;
	return false;
}

static int demo_pgd_off(u32 *out)
{
	struct mm_struct *mm;
	unsigned long v;
	unsigned long task_size_pos = 0;
	long i;
	int ret = -ENOENT;

	mm = get_task_mm(current);
	if (!mm)
		return -ENOENT;

	for (i = 0; i < 256; i++) {
		if (copy_from_kernel_nofault(&v, (char *)mm + i * 8, 8))
			break;
		if (is_task_size_cand(v)) {
			task_size_pos = i * 8;
			break;
		}
	}
	if (task_size_pos) {
		for (i = task_size_pos + 8; i < task_size_pos + 64; i += 8) {
			if (copy_from_kernel_nofault(&v, (char *)mm + i, 8))
				break;
			if (v >= 0xffff000000000000UL &&
			    !(v & (PAGE_SIZE - 1))) {
				*out = (u32)i;
				ret = 0;
				break;
			}
		}
	}
	mmput(mm);
	return ret;
}

#define SC_TEST_PATCH        0x2001
#define SC_TEST_UNPATCH      0x2002
#define SC_TEST_PATCH_DUP    0x2003
#define SC_TEST_PATCH_BAD    0x2004
#define SC_TEST_ENTRY        0x2005
#define SC_TEST_TP_GETPID    0x2006
#define SC_TEST_TP_UNGETPID  0x2007
#define SC_TEST_TP_INTERCEPT 0x2008
#define SC_TEST_TP_ENTER     0x2009
#define SC_TEST_TP_GETTID    0x200a
#define SC_TEST_TP_UNGETTID  0x200b
#define SC_TEST_TP_SLOT      0x200c
#define SC_TEST_TP_HOOKED    0x200d
#define SC_TEST_TP_REG_BAD   0x200e
#define SC_TEST_TP_REG_BIG   0x200f
#define SC_TEST_TP_REG_NULL  0x2010
#define SC_TEST_TP_ORIG_BAD  0x2011
#define SC_TEST_TP_ORIG_BIG  0x2012
#define SC_TEST_TP_ORIG_NI   0x2013
#define SC_TEST_TP_ORIG_RAW  0x2014
#define SC_TEST_TP_SELF      0x2015
#define SC_TEST_TP_CHAN      0x2016
#define SC_TEST_TP_UNSELF    0x2017
#define SC_TEST_TP_UNCHAN    0x2018
#define SC_TEST_REINIT       0x2019
#define SC_TEST_SOCK_EVENT   0x2020

static long test_handler(const struct pt_regs *regs)
{
	return 0xDEADBEEF;
}

#ifdef CONFIG_KERNSC_TP
static atomic_t tp_enter_count = ATOMIC_INIT(0);

static long demo_tp_hook(int nr, const struct pt_regs *regs)
{
	if (READ_ONCE(tp_intercept)) {
		if (nr == __NR_getpid)
			return 0x600D;
		if (nr == __NR_gettid)
			return 0x600E;
	}
	return sc_tp_orig(nr, regs);
}

static void demo_tp_enter(int id, struct pt_regs *regs)
{
	if (id == __NR_getpid || id == __NR_gettid)
		atomic_inc(&tp_enter_count);
}
#endif

static int demo_find_slot(void)
{
	unsigned long *sct;
	unsigned long ni;
	unsigned long ni_cfi;
	unsigned long v;
	int i;

	pr_info("[kerncall] layout find_slot called\n");
	sct = (unsigned long *)kr_name_to_addr("sys_call_table");
	ni = kr_name_to_addr("__arm64_sys_ni_syscall");
	ni_cfi = kr_name_to_addr("__arm64_sys_ni_syscall.cfi_jt");
	if (!sct || !ni)
		return -1;
	for (i = 0; i < 512; i++) {
		if (copy_from_kernel_nofault(&v, &sct[i], sizeof(v)))
			continue;
		if (v == ni || (ni_cfi && v == ni_cfi)) {
			pr_info("[kerncall] layout find_slot: slot=%d\n", i);
			return i;
		}
	}
	return -1;
}

static struct sc_layout g_layout = {
	.resolve = kr_name_to_addr,
	.pgd_off = demo_pgd_off,
	.find_slot = demo_find_slot,
};

static struct sc_cfg g_cfg;

#ifdef CONFIG_KERNSC_PATCH
static long demo_dispatch(long cmd, const struct pt_regs *regs, void *priv)
{
	unsigned long orig;

	switch (cmd) {
	case SC_TEST_PATCH:
		return sc_patch(249, (unsigned long)test_handler, &orig);
	case SC_TEST_UNPATCH:
		sc_unpatch(249);
		return 0;
	case SC_TEST_PATCH_DUP:
		return sc_patch(249, (unsigned long)test_handler, &orig);
	case SC_TEST_PATCH_BAD:
		return sc_patch(512, (unsigned long)test_handler, &orig);
#ifdef CONFIG_KERNSC_DISCOVER
	case SC_TEST_ENTRY:
		return sc_entry(regs->regs[2]);
#endif
#ifdef CONFIG_KERNSC_TP
	case SC_TEST_TP_GETPID:
		return sc_tp_register(__NR_getpid, demo_tp_hook);
	case SC_TEST_TP_UNGETPID:
		sc_tp_unregister(__NR_getpid);
		return 0;
	case SC_TEST_TP_GETTID:
		return sc_tp_register(__NR_gettid, demo_tp_hook);
	case SC_TEST_TP_UNGETTID:
		sc_tp_unregister(__NR_gettid);
		return 0;
	case SC_TEST_TP_INTERCEPT:
		WRITE_ONCE(tp_intercept, (int)regs->regs[2]);
		return 0;
	case SC_TEST_TP_ENTER:
		return (long)atomic_read(&tp_enter_count);
	case SC_TEST_TP_SLOT:
		return sc_tp_slot();
	case SC_TEST_TP_HOOKED:
		return sc_tp_hooked(__NR_getpid) ? 1 : 0;
	case SC_TEST_TP_REG_BAD:
		return sc_tp_register(-1, demo_tp_hook);
	case SC_TEST_TP_REG_BIG:
		return sc_tp_register(512, demo_tp_hook);
	case SC_TEST_TP_REG_NULL:
		return sc_tp_register(__NR_getpid, NULL);
	case SC_TEST_TP_ORIG_BAD:
		return sc_tp_orig(-1, regs);
	case SC_TEST_TP_ORIG_BIG:
		return sc_tp_orig(512, regs);
	case SC_TEST_TP_ORIG_NI:
		return sc_tp_orig(249, regs);
	case SC_TEST_TP_ORIG_RAW:
		return sc_tp_orig(__NR_getpid, regs);
	case SC_TEST_TP_SELF:
		return sc_tp_register(sc_tp_slot(), demo_tp_hook);
	case SC_TEST_TP_CHAN:
		return sc_tp_register(sc_get_slot(), demo_tp_hook);
	case SC_TEST_TP_UNSELF:
		sc_tp_unregister(sc_tp_slot());
		return 0;
	case SC_TEST_TP_UNCHAN:
		sc_tp_unregister(sc_get_slot());
		return 0;
#endif
	case SC_TEST_REINIT:
		return sc_init(&g_cfg);
	case SC_TEST_SOCK_EVENT:
		return sc_sock_send_event("kerncall-sock-event",
					  sizeof("kerncall-sock-event") - 1);
	default:
		return -ENOSYS;
	}
}
#else
static long demo_dispatch(long cmd, const struct pt_regs *regs, void *priv)
{
	return -ENOSYS;
}
#endif

static int __init kerncall_init(void)
{
	int ret;

	find_kallsyms_base();
	if (!kr_name_to_addr("sys_call_table")) {
		pr_warn("[kerncall] kallsyms recovery failed\n");
		return -ENODATA;
	}

	if (find_slot_mode == 1) {
		g_layout.find_slot = NULL;
		g_cfg.find_slot = NULL;
	} else {
		g_cfg.find_slot = NULL;
	}
	g_cfg.layout = &g_layout;
	g_cfg.dispatch = demo_dispatch;
	strscpy(g_cfg.key, sc_key, sizeof(g_cfg.key));
	g_cfg.priv = &g_layout;
	g_cfg.no_patch = !!no_patch;
#ifdef CONFIG_KERNSC_TP
	g_cfg.tp_enable = !!tp_enable;
	g_cfg.tp_mark_all = !!tp_mark_all;
	g_cfg.tp_on_enter = demo_tp_enter;
#endif

	ret = sc_init(&g_cfg);
	if (ret)
		return ret;

#ifdef CONFIG_KERNSC_TP
	if (tp_hook_init) {
		ret = sc_tp_register(__NR_getpid, demo_tp_hook);
		if (ret)
			pr_warn("[kerncall] tp hook init failed %d\n", ret);
	}
#endif

#ifdef CONFIG_KERNSC_SOCK
	ret = sc_sock_init(NULL);
	if (ret)
		pr_warn("[kerncall] sc_sock init failed %d\n", ret);
#endif

	pr_info("[kerncall] loaded slot=%d tp=%d sock_family=%d\n",
		sc_get_slot(),
#ifdef CONFIG_KERNSC_TP
		sc_tp_slot(),
#else
		-1,
#endif
#ifdef CONFIG_KERNSC_SOCK
		sc_sock_family()
#else
		-1
#endif
		);
	return 0;
}

static void __exit kerncall_exit(void)
{
#ifdef CONFIG_KERNSC_SOCK
	sc_sock_exit();
#endif
	sc_exit();
	pr_info("[kerncall] unloaded\n");
}

module_init(kerncall_init);
module_exit(kerncall_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kerncall: syscall channel library demo");
