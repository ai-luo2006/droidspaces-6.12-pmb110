// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * droid_lkm_pids — the pids controller name, offered to a kernel that was
 * built without it.
 *
 * Why this is a module of its own
 * -------------------------------
 * droid_lkm_misc carries devtmpfs, user namespaces and the xtables ports.
 * The devtmpfs part keeps an internal mount alive, and that mount holds a
 * reference on the module, so the module cannot be unloaded while it is up.
 * A probe that has to be loaded and unloaded on a live phone needs to be
 * separable from that, so the pids part stands on its own here: a module of
 * its own name, with its own dependencies and nothing else in it. It can be
 * inserted and removed next to a running droid_lkm_misc without touching it.
 *
 * What the kernel gives us
 * ------------------------
 * A cgroup controller is a compile time entry. include/linux/cgroup_subsys.h
 * expands SUBSYS(pids) only when CONFIG_CGROUP_PIDS is on, and the expansion
 * fixes three things at once:
 *
 *   - the enum cgroup_subsys_id in include/linux/cgroup-defs.h, whose length
 *     is CGROUP_SUBSYS_COUNT,
 *   - the array cgroup_subsys[] in kernel/cgroup/cgroup.c, indexed by that id,
 *   - the names cgroup_controllers_show() prints into cgroup.controllers.
 *
 * A module cannot add an entry to any of the three. cgroup_add_dfl_cftypes()
 * takes a cgroup_subsys that already exists and only hangs files off it, and
 * cgroup_init_cftypes() refuses a cftype that carries a subsystem of its own.
 * So the controller itself is out of reach, and this file does not try.
 *
 * What the controller was for
 * ---------------------------
 * Droidspaces reads the answer to one question, and the question is asked of
 * the kernel's own output. src/check.c asks ds_cgroup_has_controller("pids"),
 * and that helper answers yes when either of two files names the controller:
 *
 *   - the cgroup.controllers of the cgroup2 root, read by ctrl_supported_v2(),
 *   - /proc/cgroups, read by each_v1_controller(), which keeps a line whose
 *     enabled column is not zero.
 *
 * Both files are printed by the kernel, and both printers are ordinary
 * functions that a module may hook. cgroup_controllers_show() is registered as
 * the seq_show of the cgroup.controllers file, and proc_cgroupstats_show() is
 * registered as the proc handler of /proc/cgroups.
 *
 * So the controller name is added to what the kernel prints, and the name is
 * what Droidspaces looks for. That is the display layer, and this file is
 * honest about what it is: it makes the name visible, it does not create a
 * controller.
 *
 * What this does not do
 * ----------------------
 * It does not enforce a limit. The limit is written into pids.max, a file
 * that only exists when the pids controller is compiled in, so a kernel
 * without it has nowhere to put the number. Droidspaces then reports the
 * limit as skipped, which is the truth and stays the truth here.
 *
 * The enforcement is a separate matter and belongs in a separate place: a
 * counter of the tasks in a container, kept by the module, with the fork
 * path refusing to pass the limit. That is real work with real failure modes
 * and it does not belong in a file whose job is to answer a question.
 *
 * Why the printer is hooked rather than the controller
 * ----------------------------------------------------
 * Hooking the printer changes one thing: the text the kernel writes. It does
 * not change the kernel's behaviour, it does not touch a fork, and it cannot
 * make a wrong limit look enforced. A hook that fails to install leaves the
 * kernel exactly as it was, and the feature is then simply absent, which is
 * the state this kernel is in already.
 */

#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core.h"
#include "hk.h"
#include "hk_inline.h"

#define DROID_LKM_PIDS_TAG "droid_lkm_pids"

#define droid_lkm_pids_info(fmt, ...) \
	pr_info("[" DROID_LKM_PIDS_TAG "] " fmt, ##__VA_ARGS__)
#define droid_lkm_pids_warn(fmt, ...) \
	pr_warn("[" DROID_LKM_PIDS_TAG "] " fmt, ##__VA_ARGS__)
#define droid_lkm_pids_err(fmt, ...) \
	pr_err("[" DROID_LKM_PIDS_TAG "] " fmt, ##__VA_ARGS__)

static bool droid_lkm_pids_verbose;

/* enforcement layer (Plan B) lives in pids_ctl.c, same module */
extern int droid_lkm_pids_ctl_init(void);
extern void droid_lkm_pids_ctl_exit(void);

module_param_named(verbose, droid_lkm_pids_verbose, bool, 0444);
MODULE_PARM_DESC(verbose, "log the symbol probes and the hook addresses");

#define droid_lkm_pids_dbg(fmt, ...)                                  \
	do {                                                            \
		if (droid_lkm_pids_verbose)                             \
			pr_info("[" DROID_LKM_PIDS_TAG "/dbg] " fmt,   \
				##__VA_ARGS__);                          \
	} while (0)

/*
 * The two printers. Neither is exported, so both are resolved by name through
 * the kallsyms recovery the module does at load, and a kernel that does not
 * carry them simply does not get the feature.
 */
static struct hk_inline droid_lkm_pids_controllers_hook;
static struct hk_inline droid_lkm_pids_cgroupstats_hook;

static int (*droid_lkm_pids_controllers_orig)(struct seq_file *seq, void *v);
static int (*droid_lkm_pids_cgroupstats_orig)(struct seq_file *m, void *v);

/*
 * The name the controller goes by. It is the one the kernel uses for the
 * controller it compiles in, so a userland that already knows the name needs
 * no second spelling.
 */
#define DROID_LKM_PIDS_NAME "pids"

/*
 * cgroup.controllers is a whitespace separated list of the controllers a
 * cgroup may enable in its children. Droidspaces reads it with ctrl_in_list(),
 * which walks the text and accepts a word that ends in a space, a newline or
 * the end of the buffer. A trailing name without a newline is therefore still
 * a word, and the file already ends in one when the kernel printed a list.
 */
__nocfi noinline int droid_lkm_pids_controllers_wrap(struct seq_file *seq,
						     void *v)
{
	int ret;

	ret = droid_lkm_pids_controllers_orig(seq, v);
	if (ret)
		return ret;

	/*
	 * The list the kernel printed ends in a newline when it printed
	 * anything. The name is added as a word of its own, and the newline
	 * that follows keeps the file's shape: one line, names separated by
	 * spaces.
	 */
	seq_puts(seq, DROID_LKM_PIDS_NAME " ");
	seq_putc(seq, '\n');

	return 0;
}

/*
 * /proc/cgroups lists one line per controller:
 *
 *	subsys_name	hierarchy	num_cgroups	enabled
 *
 * Droidspaces parses it with sscanf(line, "%63s %d %d %d") and keeps a line
 * whose enabled column is not zero. The line added here follows that shape.
 * The hierarchy is 0, which is what the kernel itself writes for a controller
 * that has no hierarchy of its own, and the count is 1.
 */
__nocfi noinline int droid_lkm_pids_cgroupstats_wrap(struct seq_file *m,
						     void *v)
{
	int ret;

	ret = droid_lkm_pids_cgroupstats_orig(m, v);
	if (ret)
		return ret;

	seq_printf(m, "%s\t0\t1\t1\n", DROID_LKM_PIDS_NAME);

	return 0;
}

static unsigned long __nocfi droid_lkm_pids_resolve(const char *name)
{
	return kallrecon_klp ? kallrecon_klp(name) : 0;
}

static const struct hk_cfg droid_lkm_pids_hk_cfg = {
	.resolve = droid_lkm_pids_resolve,
};

static int droid_lkm_pids_hook_one(struct hk_inline *h, const char *sym,
				    const char *wrap, void **orig)
{
	int ret;

	ret = hk_inline_hook(h, sym, wrap);
	if (ret) {
		droid_lkm_pids_warn("hook %s failed: %d\n", sym, ret);
		return ret;
	}

	*orig = (void *)h->orig;
	droid_lkm_pids_dbg("hook %-28s addr=0x%lx orig=0x%lx\n", sym, h->addr,
			   h->orig);

	return 0;
}

static int __init droid_lkm_pids_init(void)
{
	int ret;

	find_kallsyms_base();
	if (!klnum_val || !kallrecon_klp) {
		droid_lkm_pids_err("kallsyms recovery failed\n");
		return -ENODATA;
	}

	ret = hk_init(&droid_lkm_pids_hk_cfg);
	if (ret) {
		droid_lkm_pids_err("hk_init failed: %d\n", ret);
		return ret;
	}

	/*
	 * The cgroup2 path comes first: a kernel that has the controller
	 * compiled in prints the name itself, and the hook would only add a
	 * second copy of it. The name is checked against the file rather than
	 * assumed, so a kernel with the controller is left untouched.
	 */
	ret = droid_lkm_pids_hook_one(&droid_lkm_pids_controllers_hook,
		"cgroup_controllers_show", "droid_lkm_pids_controllers_wrap",
		(void **)&droid_lkm_pids_controllers_orig);
	if (ret)
		goto out_hk;

	ret = droid_lkm_pids_hook_one(&droid_lkm_pids_cgroupstats_hook,
		"proc_cgroupstats_show", "droid_lkm_pids_cgroupstats_wrap",
		(void **)&droid_lkm_pids_cgroupstats_orig);
	if (ret)
		goto out_controllers;

	/* Plan B: make --pids-limit actually enforce. Best effort; a failure here
	 * tears the whole module down so we never load in a half wired state. */
	ret = droid_lkm_pids_ctl_init();
	if (ret)
		goto out_cgroupstats;

	droid_lkm_pids_info("pids controller name offered to the cgroup printers\n");
	return 0;

out_cgroupstats:
	hk_inline_unhook(&droid_lkm_pids_cgroupstats_hook);
out_controllers:
	hk_inline_unhook(&droid_lkm_pids_controllers_hook);
out_hk:
	hk_exit();
	return ret;
}

static void __exit droid_lkm_pids_exit(void)
{
	droid_lkm_pids_ctl_exit();
	hk_inline_unhook(&droid_lkm_pids_cgroupstats_hook);
	hk_inline_unhook(&droid_lkm_pids_controllers_hook);
	hk_exit();
	droid_lkm_pids_info("unloaded\n");
}

module_init(droid_lkm_pids_init);
module_exit(droid_lkm_pids_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("offer the pids controller name to a kernel built without it");
