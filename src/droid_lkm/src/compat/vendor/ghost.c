// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <linux/threads.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "core.h"
#include "ds_compat.h"
#include "ds_hook.h"
#include "ghost.h"
#include "vendor_syms.h"
#include "hk_inline.h"

/* 6.18 builds with -Wmissing-prototypes */
__nocfi noinline struct task_struct *dlc_ftbv_wrap(pid_t vnr);

#define DLC_GHOST_TASK_COMM	"ghost-task-sentinel"
#define DLC_GHOST_LOG_MAX	3

static bool dlc_ghost_enable = true;
module_param_named(ghost, dlc_ghost_enable, bool, 0444);
MODULE_PARM_DESC(ghost, "hand out a ghost task when find_task_by_vpid misses, default 1");

static char *dlc_ghost_match = "oplus_";
module_param_named(ghost_match, dlc_ghost_match, charp, 0444);
MODULE_PARM_DESC(ghost_match, "caller module name prefix, * matches all, default oplus_");

static typeof(&find_task_by_vpid) dlc_ftbv_orig;
static typeof(&__module_address) dlc_module_address;
static struct ds_hook dlc_ftbv_hook;
static struct task_struct dlc_ghost_task;
static bool dlc_ghost_ready;
static unsigned int dlc_ghost_hits;

static bool dlc_ghost_match_module(const char *name)
{
	size_t n;

	if (!dlc_ghost_match || !*dlc_ghost_match)
		return false;
	if (dlc_ghost_match[0] == '*' && !dlc_ghost_match[1])
		return true;
	n = strlen(dlc_ghost_match);
	return strncmp(name, dlc_ghost_match, n) == 0;
}

// _RET_IP_ must be read before calling orig
__nocfi noinline struct task_struct *dlc_ftbv_wrap(pid_t vnr)
{
	unsigned long ret_ip = (unsigned long)_RET_IP_;
	struct task_struct *task;
	struct module *mod;
	bool match = false;

	if (unlikely(!dlc_ftbv_orig))
		return NULL;

	task = dlc_ftbv_orig(vnr);
	ds_hook_leave(&dlc_ftbv_hook);
	if (likely(task))
		return task;

	if (!dlc_ghost_ready)
		return task;

	if (dlc_module_address) {
		preempt_disable();
		mod = dlc_module_address(ret_ip);
		if (mod)
			match = dlc_ghost_match_module(mod->name);
		preempt_enable();
		if (!match)
			return task;
		dlc_ghost_hits++;
		if (dlc_ghost_hits <= DLC_GHOST_LOG_MAX)
			pr_info("[droid_lkm_vendor] ghost: pid=%d caller=%pS -> %s\n",
				(int)vnr, (void *)ret_ip, DLC_GHOST_TASK_COMM);
		return &dlc_ghost_task;
	}

	return task;
}

enum {
	DLC_GHOST_F_PID,
	DLC_GHOST_F_TGID,
	DLC_GHOST_F_COMM,
	DLC_GHOST_F_USAGE,
	DLC_GHOST_F_TASKS,
	DLC_GHOST_F_CHILDREN,
	DLC_GHOST_F_SIBLING,
	DLC_GHOST_F_THREAD_NODE,
	DLC_GHOST_F_PTRACED,
	DLC_GHOST_F_PTRACE_ENTRY,
	DLC_GHOST_F_COUNT
};

static struct ds_compat_field dlc_ghost_fields[DLC_GHOST_F_COUNT] = {
	[DLC_GHOST_F_PID] = { .type = "task_struct", .member = "pid" },
	[DLC_GHOST_F_TGID] = { .type = "task_struct", .member = "tgid" },
	[DLC_GHOST_F_COMM] = { .type = "task_struct", .member = "comm" },
	[DLC_GHOST_F_USAGE] = { .type = "task_struct", .member = "usage" },
	[DLC_GHOST_F_TASKS] = { .type = "task_struct", .member = "tasks" },
	[DLC_GHOST_F_CHILDREN] = { .type = "task_struct", .member = "children" },
	[DLC_GHOST_F_SIBLING] = { .type = "task_struct", .member = "sibling" },
	[DLC_GHOST_F_THREAD_NODE] = { .type = "task_struct", .member = "thread_node" },
	[DLC_GHOST_F_PTRACED] = { .type = "task_struct", .member = "ptraced" },
	[DLC_GHOST_F_PTRACE_ENTRY] = { .type = "task_struct", .member = "ptrace_entry" },
};

static struct ds_compat_type dlc_ghost_types[] = {
	{ .name = "task_struct" },
};

static struct ds_compat_layout dlc_ghost_layout_table = {
	.module = "droid_lkm_vendor",
	.fields = dlc_ghost_fields,
	.nfields = ARRAY_SIZE(dlc_ghost_fields),
	.types = dlc_ghost_types,
	.ntypes = ARRAY_SIZE(dlc_ghost_types),
};

static u32 dlc_ghost_size;

// offsets come from the running kernel BTF
// a build offset would write pid or comm into whatever the kernel keeps there
static int dlc_ghost_layout(void)
{
	if (!ds_compat_layout_init(&dlc_ghost_layout_table))
		return -ENODATA;

	dlc_ghost_size = dlc_ghost_types[0].live;
	return 0;
}

static void *dlc_ghost_at(unsigned int field)
{
	return (char *)&dlc_ghost_task + dlc_ghost_fields[field].live;
}

// ghost is a shallow copy of init_task, list heads re-init to self
static int dlc_ghost_build(void)
{
	unsigned long init_task_addr = dlc_vendor_syms.init_task;
	unsigned int copy, i;
	int ret;

	if (!init_task_addr)
		return -ENOENT;

	ret = dlc_ghost_layout();
	if (ret) {
		pr_err("[droid_lkm_vendor] ghost: no task_struct offsets for the running kernel (%d)\n",
		       ret);
		return ret;
	}

	// the running kernel's task_struct may be smaller than this build's
	copy = sizeof(dlc_ghost_task);
	if (dlc_ghost_size < copy)
		copy = dlc_ghost_size;
	memcpy(&dlc_ghost_task, (void *)init_task_addr, copy);

	for (i = DLC_GHOST_F_TASKS; i <= DLC_GHOST_F_PTRACE_ENTRY; i++)
		INIT_LIST_HEAD(dlc_ghost_at(i));

	strscpy(dlc_ghost_at(DLC_GHOST_F_COMM), DLC_GHOST_TASK_COMM, TASK_COMM_LEN);
	// vendors index PID_MAX_DEFAULT sized arrays by p->pid
	// init_task.thread_pid is init_struct_pid nr 0 so the vnr helpers agree
	*(pid_t *)dlc_ghost_at(DLC_GHOST_F_PID) = 0;
	*(pid_t *)dlc_ghost_at(DLC_GHOST_F_TGID) = 0;
	refcount_set((refcount_t *)dlc_ghost_at(DLC_GHOST_F_USAGE), 1000);
	dlc_ghost_ready = true;
	return 0;
}

// vendor tables are PID_MAX_DEFAULT entries indexed by task->pid
// 0 caps only when such a module is loaded
// -1 never  N caps to N
static int dlc_pid_max_cap;
module_param_named(pid_max_cap, dlc_pid_max_cap, int, 0444);
MODULE_PARM_DESC(pid_max_cap,
	"0 caps pid_max only when a pid indexed vendor module is loaded, -1 never, N caps to N");

static const char *const dlc_pid_indexed_modules[] = {
	"oplus_bsp_sched_assist",
	"oplus_bsp_task_sched",
	"oplus_bsp_schedinfo",
	"oplus_bsp_schedtune",
	"oplus_bsp_sched_ext",
	"oplus_bsp_uxmem_opt",
};

/* sysfs is the one place every branch agrees on, so ask it through kern_path */
static bool dlc_module_loaded(const char *name)
{
	int (*kern_path_fn)(const char *, unsigned int, struct path *);
	void (*path_put_fn)(const struct path *);
	char path[64];
	struct path p;

	kern_path_fn = dlc_vendor_syms.kern_path;
	path_put_fn = dlc_vendor_syms.path_put;
	if (!kern_path_fn || !path_put_fn)
		return false;

	snprintf(path, sizeof(path), "/sys/module/%s", name);
	if (kern_path_fn(path, 0, &p))
		return false;
	path_put_fn(&p);
	return true;
}

#define DLC_PID_MAX_LOG_MAX	3
/*
 * Measured on PMB110: a container writes 4194304 and the cap lands 0.6 s
 * later with a 1 s period, so the window is as wide as the period. pid only
 * has to climb 68 places inside it to leave the vendor range, which is a few
 * milliseconds of forking. 200 ms shrinks the window by 5x for one integer
 * comparison per tick.
 */
#define DLC_PID_MAX_WATCH_MS	200

static int dlc_pid_max_caps;

static void dlc_ghost_audit_pid_max(void)
{
	int *pid_max_addr = dlc_vendor_syms.pid_max;
	int value, cap = dlc_pid_max_cap;
	int i;

	if (!pid_max_addr || cap < 0)
		return;

	value = READ_ONCE(*pid_max_addr);

	/* an explicit cap is enforced as given, whatever the default happens to be */
	if (cap > 0) {
		if (value <= cap)
			return;
		WRITE_ONCE(*pid_max_addr, cap);
		if (dlc_pid_max_caps < DLC_PID_MAX_LOG_MAX)
			pr_warn("[droid_lkm_vendor] pid_max %d -> %d by module parameter\n",
				value, cap);
		dlc_pid_max_caps++;
		return;
	}

	if (value <= PID_MAX_DEFAULT)
		return;

	/*
	 * Auto mode. A container raises pid_max above PID_MAX_DEFAULT so that
	 * its forks stop wrapping, and the vendor arrays indexed by task->pid
	 * are sized for PID_MAX_DEFAULT. Name the module once for the log, then
	 * cap regardless: kern_path may be unresolved, and a missed cap is a
	 * panic while an unneeded cap only costs pid range nothing uses.
	 */
	if (!dlc_pid_max_caps) {
		for (i = 0; i < ARRAY_SIZE(dlc_pid_indexed_modules); i++) {
			if (dlc_module_loaded(dlc_pid_indexed_modules[i])) {
				pr_warn("[droid_lkm_vendor] pid_max=%d with %s loaded: pid indexed arrays are sized for %d\n",
					value, dlc_pid_indexed_modules[i],
					PID_MAX_DEFAULT);
				break;
			}
		}
	}

	WRITE_ONCE(*pid_max_addr, PID_MAX_DEFAULT);
	if (dlc_pid_max_caps < DLC_PID_MAX_LOG_MAX)
		pr_warn("[droid_lkm_vendor] pid_max %d -> %d, pids wrap inside the range vendor arrays cover\n",
			value, PID_MAX_DEFAULT);
	dlc_pid_max_caps++;
}

static struct delayed_work dlc_pid_max_work;
static bool dlc_pid_max_watch;

static void dlc_pid_max_work_fn(struct work_struct *work)
{
	dlc_ghost_audit_pid_max();

	if (dlc_pid_max_watch)
		schedule_delayed_work(&dlc_pid_max_work,
				      msecs_to_jiffies(DLC_PID_MAX_WATCH_MS));
}

/*
 * The cap used to run once from init, before any container existed, and a
 * container raises pid_max long after that. Watch it instead of sampling it.
 */
static void dlc_pid_max_watch_start(void)
{
	if (dlc_pid_max_cap < 0)
		return;

	if (!dlc_vendor_syms.pid_max) {
		pr_warn("[droid_lkm_vendor] pid_max not resolved, no cap applied\n");
		return;
	}

	dlc_ghost_audit_pid_max();

	INIT_DELAYED_WORK(&dlc_pid_max_work, dlc_pid_max_work_fn);
	dlc_pid_max_watch = true;
	schedule_delayed_work(&dlc_pid_max_work,
			      msecs_to_jiffies(DLC_PID_MAX_WATCH_MS));
}

static void dlc_pid_max_watch_stop(void)
{
	if (!dlc_pid_max_watch)
		return;

	dlc_pid_max_watch = false;
	cancel_delayed_work_sync(&dlc_pid_max_work);
}

/*
 * Same reason as the ddl guard: every way this fix can quietly do nothing -
 * pid_max not resolved, the module parameter set to -1, the cap never firing
 * because no container ever raised it - looks identical from the outside,
 * which is "the module loaded, 6/6". Say it here instead.
 */
static int dlc_pid_max_show(struct seq_file *m, void *v)
{
	int *addr = dlc_vendor_syms.pid_max;

	if (dlc_pid_max_cap < 0)
		seq_puts(m, "state     off (pid_max_cap=-1)\n");
	else if (!addr)
		seq_puts(m, "state     symbol-missing (pid_max not resolved)\n");
	else if (dlc_pid_max_watch)
		seq_printf(m, "state     watching (%d ms)\n", DLC_PID_MAX_WATCH_MS);
	else
		seq_puts(m, "state     idle\n");

	seq_printf(m, "target    %d\n", PID_MAX_DEFAULT);
	seq_printf(m, "caps      %d\n", dlc_pid_max_caps);
	if (addr)
		seq_printf(m, "pid_max   %d\n", READ_ONCE(*addr));
	else
		seq_puts(m, "pid_max   unresolved\n");
	seq_printf(m, "ghost     %s\n", dlc_ghost_ready ? "hooked" : "off");
	return 0;
}

static int dlc_pid_max_open(struct inode *inode, struct file *file)
{
	return single_open(file, dlc_pid_max_show, NULL);
}

static const struct proc_ops dlc_pid_max_pops = {
	.proc_open	= dlc_pid_max_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static struct proc_dir_entry *dlc_pid_max_pde;

void dlc_ghost_exit(void)
{
	if (dlc_pid_max_pde) {
		proc_remove(dlc_pid_max_pde);
		dlc_pid_max_pde = NULL;
	}

	dlc_pid_max_watch_stop();
	ds_hook_remove(&dlc_ftbv_hook);
	dlc_ghost_ready = false;
}

int dlc_ghost_init(void)
{
	int ret;

	if (!dlc_pid_max_pde)
		dlc_pid_max_pde = proc_create("pid_max_guard", 0444, NULL,
					      &dlc_pid_max_pops);

	/* the cap stands on its own, arm it even when the ghost task is off */
	dlc_pid_max_watch_start();

	if (!dlc_ghost_enable)
		return 0;

	ret = dlc_ghost_build();
	if (ret) {
		pr_err("[droid_lkm_vendor] ghost: init_task not found\n");
		goto fail;
	}

	dlc_module_address = (typeof(dlc_module_address))dlc_vendor_syms.module_address;
	if (!dlc_module_address) {
		pr_err("[droid_lkm_vendor] ghost: __module_address not found\n");
		dlc_ghost_ready = false;
		ret = -ENOENT;
		goto fail;
	}

	/*
	 * the engine patches the end of a branch chain when the entry is a thunk,
	 * and on a +lto kernel find_task_by_vpid is one. the trampoline of that
	 * build loops back into the patched entry, so a call recurses until the
	 * kernel stack is gone. judge the entry first and refuse a shape the
	 * engine cannot drive, the feature degrades instead of panicking
	 */
	{
		struct hk_inline_probe probe;

		memset(&probe, 0, sizeof(probe));
		if (!hk_inline_probe("find_task_by_vpid", &probe) &&
		    probe.state != HK_INLINE_PLAIN) {
			pr_err("[droid_lkm_vendor] ghost: find_task_by_vpid entry state %d (%s) target 0x%lx, inline hook refused\n",
			       probe.state, probe.reason ? probe.reason : "?",
			       probe.target);
			dlc_ghost_ready = false;
			ret = -EOPNOTSUPP;
			goto fail;
		}
	}

	ret = ds_hook_install(&dlc_ftbv_hook, "find_task_by_vpid", "dlc_ftbv_wrap");
	if (ret) {
		pr_err("[droid_lkm_vendor] ghost: hook find_task_by_vpid failed %d\n", ret);
		dlc_ghost_ready = false;
		goto fail;
	}
	dlc_ftbv_orig = (typeof(dlc_ftbv_orig))dlc_ftbv_hook.orig;
	pr_info("[droid_lkm_vendor] ghost: find_task_by_vpid hooked, match=%s\n",
		dlc_ghost_match ? dlc_ghost_match : "");
	return 0;

fail:
	dlc_pid_max_watch_stop();
	return ret;
}
