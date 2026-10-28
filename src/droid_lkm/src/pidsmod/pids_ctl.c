// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * droid_lkm_pids — enforcement layer (Plan B)
 *
 * The display layer (pids_main.c) makes the kernel print the "pids" controller
 * name so Droidspaces reports "Process limit support". This file makes the limit
 * real:
 *
 *   B1  a pids.max file is hung off the memory cgroup controller through
 *       cgroup_add_dfl_cftypes(); the kernel creates it, Droidspaces writes the
 *       number there and we keep it. pids.current is exposed next to it so the
 *       counter can be read back.
 *
 *   B2  the fork path refuses to pass the limit. A per-cgroup atomic counter is
 *       charged by a kprobe on cgroup_can_fork() and uncharged by one on
 *       cgroup_exit(). Both walk the cgroup ancestors, so a limit set on the
 *       container constrains the whole subtree, matching the real pids
 *       semantics.
 *
 *   B3  Droidspaces writes "+memory +cpu +pids" to cgroup.subtree_control. The
 *       kernel has no pids controller, so the whole write fails with EINVAL and
 *       takes +memory/+cpu down with it — which is what left the container
 *       cgroup without memory and therefore without pids.max. A kprobe blanks
 *       the pids token in the write buffer before the kernel parses it.
 *
 * Why kprobes and not inline hooks
 * --------------------------------
 * On this device the inline hooks installed cleanly and then never ran. The two
 * display hooks, which are reached through a .seq_show pointer, do work; every
 * directly called target in this file stayed silent — cgroup.subtree_control
 * kept rejecting "+pids", and pids.current sat at 0 through hundreds of forks.
 * cgroup_procs_write was worse than silent: its trampoline is a BRK trap and it
 * took the phone down 22ms after load. register_kprobe arms a breakpoint under
 * text_mutex and cpus_read_lock, so it does not depend on relocating the
 * target's entry at all, and it is the mechanism the kernel itself maintains.
 *
 * A kprobe observes a call rather than replacing it, so the fork probe refuses
 * by writing the return value into x0 and returning through the link register;
 * the probed function never runs for that call.
 *
 * Layout safety
 * -------------
 * struct css_set begins with subsys[CGROUP_SUBSYS_COUNT], so the offset of
 * dfl_cgrp depends on how many controllers the running kernel was built with.
 * The count is recovered from cgroup_subsys[] at runtime and the offset is
 * corrected by the difference, then verified: init_css_set.dfl_cgrp must equal
 * &cgrp_dfl_root.cgrp, and init_task.cgroups must equal &init_css_set. If either
 * check fails the fork and exit probes are never installed, so a wrong offset
 * costs the feature instead of reading random memory.
 *
 * Every probe is best effort: a failed install leaves the kernel exactly as it
 * was and the feature is simply absent, never a crash.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include <linux/cgroup.h>
#include <linux/cgroup-defs.h>
#include <linux/kernfs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/uaccess.h>

#include "core.h"
#include "hk.h"
#include "hk_inline.h"
#include "hk_kprobe.h"

#define PIDS_CTL_TAG "droid_lkm_pids_ctl"

#define pids_ctl_info(fmt, ...)  pr_info("[" PIDS_CTL_TAG "] " fmt, ##__VA_ARGS__)
#define pids_ctl_warn(fmt, ...)  pr_warn("[" PIDS_CTL_TAG "] " fmt, ##__VA_ARGS__)
#define pids_ctl_err(fmt, ...)   pr_err("[" PIDS_CTL_TAG "] " fmt, ##__VA_ARGS__)

/* per-cgroup state, keyed by the cgroup pointer in pids_xa */
struct pids_priv {
	atomic64_t counter;	/* this cgroup plus every descendant, like pids.current */
	atomic64_t limit;	/* 0 == disabled; S64_MAX == "max" (unlimited) */
};

static DEFINE_XARRAY(pids_xa);

static struct pids_priv *pids_find(struct cgroup *cgrp)
{
	return xa_load(&pids_xa, (unsigned long)cgrp);
}

#define pids_next_ancestor(c)	((c)->self.parent ? (c)->self.parent->cgroup : NULL)

/*
 * The counter is hierarchical: every fork charges the whole ancestor chain, so
 * the value on the container is the total of the container and everything
 * below it. That is what the real controller reports in pids.current -- a
 * container's processes live in child cgroups (ds-enter, init.scope,
 * system.slice ...), not in the container directory itself.
 *
 * Accounting points, chosen because they pair up exactly:
 *   cgroup_post_fork()  once per task that was actually created
 *   cgroup_exit()       once per task that goes away
 * Charging on cgroup_can_fork() instead would leak one count for every fork
 * that fails later on, since the matching cgroup_cancel_fork() walks the
 * kernel's own subsystem list and never reaches us.
 */

/* -EAGAIN if one more task would break a limit. Leaves the counters alone. */
static int pids_check(struct cgroup *cgrp)
{
	struct cgroup *c;

	for (c = cgrp; c; c = pids_next_ancestor(c)) {
		struct pids_priv *p = pids_find(c);
		s64 limit;

		if (!p)
			continue;
		limit = atomic64_read(&p->limit);
		if (limit <= 0 || limit == S64_MAX)
			continue;
		if (atomic64_read(&p->counter) + 1 > limit)
			return -EAGAIN;
	}
	return 0;
}

static void pids_charge(struct cgroup *cgrp, int add)
{
	struct cgroup *c;

	for (c = cgrp; c; c = pids_next_ancestor(c)) {
		struct pids_priv *p = pids_find(c);
		long v;

		if (!p)
			continue;
		if (atomic64_read(&p->limit) <= 0)
			continue;
		v = atomic64_add_return(add, &p->counter);
		if (v < 0)
			atomic64_set(&p->counter, 0);
	}
}

/* ------------------------------------------------------------ layout ---- */
static unsigned long ctl_css_dfl_cgrp_off;
static unsigned long ctl_task_cgroups_off;
static unsigned long ctl_kargs_cset_off;
static unsigned long ctl_kn_priv_off;
static bool ctl_layout_ok;

static bool ctl_read_ptr(unsigned long addr, unsigned long *out)
{
	return copy_from_kernel_nofault(out, (void *)addr, sizeof(*out)) == 0;
}

static bool ctl_is_kptr(unsigned long v)
{
	return v && (v & 0xffff000000000000UL) == 0xffff000000000000UL;
}

static void ctl_layout_init(void)
{
	unsigned long subsys, init_css, dfl_root, init_task_addr;
	unsigned long expect_root, v;
	int phone_count = 0;
	int i;

	subsys = kallrecon_klp("cgroup_subsys");
	init_css = kallrecon_klp("init_css_set");
	dfl_root = kallrecon_klp("cgrp_dfl_root");
	init_task_addr = kallrecon_klp("init_task");

	if (!subsys || !init_css || !dfl_root || !init_task_addr) {
		pids_ctl_warn("layout symbols missing "
			      "(subsys=%d init_css_set=%d cgrp_dfl_root=%d "
			      "init_task=%d)\n",
			      !!subsys, !!init_css, !!dfl_root, !!init_task_addr);
		return;
	}

	for (i = 0; i < 64; i++) {
		if (!ctl_read_ptr(subsys + i * 8, &v))
			break;
		if (!ctl_is_kptr(v))
			break;
		phone_count++;
	}

	ctl_css_dfl_cgrp_off = offsetof(struct css_set, dfl_cgrp) +
		(long)(phone_count - (int)CGROUP_SUBSYS_COUNT) * 8;
	ctl_task_cgroups_off = offsetof(struct task_struct, cgroups);
	ctl_kargs_cset_off = offsetof(struct kernel_clone_args, cset);
	ctl_kn_priv_off = offsetof(struct kernfs_node, priv);

	expect_root = dfl_root + offsetof(struct cgroup_root, cgrp);
	if (!ctl_read_ptr(init_css + ctl_css_dfl_cgrp_off, &v) ||
	    v != expect_root) {
		pids_ctl_warn("css_set.dfl_cgrp unverified: controllers=%d "
			      "off=%lu got=0x%lx want=0x%lx\n",
			      phone_count, ctl_css_dfl_cgrp_off, v, expect_root);
		return;
	}

	if (!ctl_read_ptr(init_task_addr + ctl_task_cgroups_off, &v) ||
	    v != init_css) {
		pids_ctl_warn("task_struct.cgroups unverified: off=%lu "
			      "got=0x%lx want=0x%lx\n",
			      ctl_task_cgroups_off, v, init_css);
		return;
	}

	ctl_layout_ok = true;
	pids_ctl_info("layout verified: controllers=%d (built against %d)\n",
		      phone_count, (int)CGROUP_SUBSYS_COUNT);
}

static struct cgroup *ctl_css_dfl_cgrp(struct css_set *cset)
{
	unsigned long v;

	if (!ctl_layout_ok || !cset)
		return NULL;
	if (!ctl_read_ptr((unsigned long)cset + ctl_css_dfl_cgrp_off, &v))
		return NULL;
	return (struct cgroup *)v;
}

static struct cgroup *ctl_task_dfl_cgrp(struct task_struct *tsk)
{
	unsigned long v;

	if (!ctl_layout_ok || !tsk)
		return NULL;
	if (!ctl_read_ptr((unsigned long)tsk + ctl_task_cgroups_off, &v))
		return NULL;
	return ctl_css_dfl_cgrp((struct css_set *)v);
}

/* ------------------------------------------------------- diagnostics ---- */
/*
 * How many times each probe body actually ran. Read them back through
 * pids.stats: a counter that stays at zero while the kernel is visibly doing
 * the work means the probe was armed at an address the kernel never reaches,
 * which is exactly the failure this file has to tell apart from a probe that
 * runs but decides not to act.
 */
static atomic_t ctl_n_subtree;
static atomic_t ctl_n_strip;
static atomic_t ctl_n_rewrite;
static atomic_t ctl_n_fork;
static atomic_t ctl_n_charged;
static atomic_t ctl_n_refused;
static atomic_t ctl_n_rmdir;

/* --------------------------------------------------------------- B3 ----- */
/*
 * cgroup.subtree_control write: blank out the tokens the kernel cannot accept
 * before it parses the buffer. The kernel splits on spaces and skips empty
 * tokens, so turning "+cpu" into spaces leaves a plain "+memory" write that
 * succeeds, instead of one EINVAL that drops memory along with cpu.
 *
 * The list is not arbitrary. A controller mounted on a v1 hierarchy is not
 * available on the v2 root, and on these phones /proc/cgroups shows only
 * memory at hierarchy 0 while cpu, cpuset, cpuacct, blkio, freezer and the
 * net_* ones all sit on v1 hierarchies 1-8. pids is the one we invent. So
 * "+memory +cpu +pids" has to become "+memory" or nothing gets enabled at all,
 * which is why Droidspaces never found a pids.max to write to.
 */
static const char *const ctl_strip_names[] = { "pids", "cpu" };

static void ctl_blank_name(char *buf, size_t nbytes, const char *name)
{
	size_t nlen = strlen(name);
	size_t i;

	for (i = 0; i + nlen <= nbytes; i++) {
		size_t start = i;
		size_t end = i + nlen;

		if (memcmp(buf + i, name, nlen) != 0)
			continue;

		/* "cpuset" must not lose its cpu prefix: require a token end */
		if (end < nbytes && buf[end] != ' ' && buf[end] != '\n' &&
		    buf[end] != '\t')
			continue;

		/* the +/- in front of the name belongs to the same token */
		if (start > 0 && (buf[start - 1] == '+' || buf[start - 1] == '-'))
			start--;

		/*
		 * The buffer is kernel memory handed to us by the probed
		 * function itself, so it is live. copy_to_kernel_nofault()
		 * would be nicer but it is not exported on every vendor
		 * kernel, and an unresolvable symbol kills the whole module.
		 */
		memset(buf + start, ' ', end - start);
		atomic_inc(&ctl_n_strip);
		i = end - 1;
	}
}

/*
 * Where the rewritten token list goes. The buffer kernfs hands in is sized for
 * exactly what userspace wrote, so "+pids" arrives in six bytes and "+memory"
 * does not fit. Pointing the probed function at a buffer of our own is what
 * lets a five byte request turn into a seven byte one.
 */
static DEFINE_PER_CPU(char [32], ctl_rw_buf);

/*
 * Enabling memory on a cgroup is sticky: once its subtree_control holds
 * memory it keeps it until the directory goes away. So the translation only
 * has to happen once per cgroup, and repeating it is what made the second
 * write come back EFAULT. Remember the cgroups we already answered.
 */
#define CTL_REWRITTEN_MAX 8
static unsigned long ctl_rewritten[CTL_REWRITTEN_MAX];
static int ctl_n_rewritten;

static bool ctl_already_rewritten(unsigned long cgrp)
{
	int i;

	for (i = 0; i < ctl_n_rewritten; i++)
		if (ctl_rewritten[i] == cgrp)
			return true;
	return false;
}

static void ctl_note_rewritten(unsigned long cgrp)
{
	if (ctl_n_rewritten >= CTL_REWRITTEN_MAX || !cgrp)
		return;
	ctl_rewritten[ctl_n_rewritten++] = cgrp;
}

static struct hk_kprobe ctl_subtree_probe;

static int ctl_subtree_pre(struct kprobe *p, struct pt_regs *regs)
{
	char *buf = (char *)regs->regs[1];
	size_t nbytes = (size_t)regs->regs[2];
	struct kernfs_open_file *of = (struct kernfs_open_file *)regs->regs[0];
	unsigned long cgrp = 0;
	bool saw_pids = false;
	bool has_enable = false;
	size_t i;
	int j;

	atomic_inc(&ctl_n_subtree);

	if (!buf || !nbytes || nbytes > PAGE_SIZE)
		return 0;

	if (of && of->kn && of->kn->parent)
		cgrp = (unsigned long)of->kn->parent->priv;

	/* what Droidspaces asked for, before we blank any of it */
	for (i = 0; i + 4 <= nbytes; i++) {
		if (memcmp(buf + i, "pids", 4) != 0)
			continue;
		if (i > 0 && (buf[i - 1] == '+' || buf[i - 1] == '-')) {
			saw_pids = true;
			break;
		}
	}

	for (j = 0; j < ARRAY_SIZE(ctl_strip_names); j++)
		ctl_blank_name(buf, nbytes, ctl_strip_names[j]);

	for (i = 0; i < nbytes; i++) {
		if (buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\t' &&
		    buf[i] != '\0') {
			has_enable = true;
			break;
		}
	}

	/* Something still stands, let the kernel parse what is left. */
	if (has_enable)
		return 0;

	/*
	 * A container configured with --pids-limit and no --memory-limit makes
	 * Droidspaces send exactly "+pids": it only offers +memory when the
	 * config asks for a memory limit. Blanking it would enable nothing, so
	 * the container cgroup would never get a memory css and pids.max would
	 * never appear there. Answer the request with the controller that is
	 * actually available, which is the one pids.max is hung off.
	 */
	if (!saw_pids)
		return 0;

	/* already enabled on this cgroup by an earlier write: doing it again
	 * is what made Droidspaces report "Bad address" */
	if (cgrp && ctl_already_rewritten(cgrp))
		return 0;

	preempt_disable();
	{
		char *nb = this_cpu_ptr(ctl_rw_buf);

		memcpy(nb, "+memory", 7);
		nb[7] = '\0';
		regs->regs[1] = (unsigned long)nb;
		regs->regs[2] = 7;
	}
	preempt_enable();
	ctl_note_rewritten(cgrp);
	atomic_inc(&ctl_n_rewrite);

	return 0;
}

/* --------------------------------------------------------------- B2 ----- */
/*
 * cgroup_can_fork: charge the child's cgroup. A refusal is delivered by writing
 * the return value into x0 and returning through the link register, so the
 * probed function never runs for this call and copy_process aborts the fork the
 * same way the real pids controller does.
 */
static struct hk_kprobe ctl_can_fork_probe;

static int ctl_can_fork_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct cgroup *cgrp;

	atomic_inc(&ctl_n_fork);

	/*
	 * Not kargs->cset: this probe runs on entry, and cgroup_can_fork()
	 * fills that in itself by calling cgroup_css_set_fork() once inside,
	 * so at this point it still holds whatever copy_process left there.
	 * The child is going to inherit the forking task's cgroup, and
	 * task_struct.cgroups is one of the two offsets ctl_layout_init()
	 * verified, so charge current instead.
	 */
	if (!ctl_layout_ok)
		return 0;

	cgrp = ctl_task_dfl_cgrp(current);
	if (!cgrp)
		return 0;
	atomic_inc(&ctl_n_charged);
	if (!pids_check(cgrp))
		return 0;

	atomic_inc(&ctl_n_refused);
	regs->regs[0] = (unsigned long)(-EAGAIN);
	hk_regs_return(regs);
	return 1;
}

/*
 * cgroup_post_fork: charge the child. By the time this runs the fork has
 * succeeded and the child already carries its cgroup, so this pairs up one to
 * one with cgroup_exit().
 */
static struct hk_kprobe ctl_fork_probe;

static int ctl_fork_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct kernel_clone_args *kargs =
		(struct kernel_clone_args *)regs->regs[1];
	unsigned long v;
	struct cgroup *cgrp;

	if (!ctl_layout_ok || !kargs)
		return 0;

	/*
	 * Not child->cgroups: this probe runs on entry and cgroup_post_fork()
	 * assigns that pointer itself a few instructions later, so it still
	 * holds whatever the child was born with. kargs->cset was filled in
	 * earlier, inside cgroup_can_fork()'s cgroup_css_set_fork(), and is
	 * the css_set the child is about to be attached to.
	 */
	if (!ctl_read_ptr((unsigned long)kargs + ctl_kargs_cset_off, &v))
		return 0;

	cgrp = ctl_css_dfl_cgrp((struct css_set *)v);
	if (cgrp)
		pids_charge(cgrp, 1);
	return 0;
}

/* cgroup_exit: uncharge the task that is going away. */
static struct hk_kprobe ctl_exit_probe;

static int ctl_exit_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *tsk = (struct task_struct *)regs->regs[0];
	struct cgroup *cgrp;

	if (!ctl_layout_ok || !tsk)
		return 0;

	cgrp = ctl_task_dfl_cgrp(tsk);
	if (cgrp)
		pids_charge(cgrp, -1);
	return 0;
}

/*
 * cgroup_rmdir: drop the per-cgroup state so a reused pointer cannot enforce a
 * stale limit. Taken at the entry, so a directory that then fails to go away
 * loses its limit; that is a missing restriction, never a wrong one.
 */
static struct hk_kprobe ctl_rmdir_probe;

static int ctl_rmdir_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct kernfs_node *kn = (struct kernfs_node *)regs->regs[0];
	unsigned long v;

	atomic_inc(&ctl_n_rmdir);
	if (!kn)
		return 0;
	if (!ctl_read_ptr((unsigned long)kn + ctl_kn_priv_off, &v))
		return 0;

	xa_erase(&pids_xa, v);
	return 0;
}

/* --------------------------------------------------------------- B1 ----- */
static u64 pids_max_read_u64(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct cgroup *cgrp = css->cgroup;
	struct pids_priv *p = pids_find(cgrp);

	return p ? (u64)atomic64_read(&p->limit) : 0;
}

static ssize_t pids_max_write(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, loff_t off)
{
	struct cgroup *cgrp = of->kn->parent->priv;
	char tmp[32];
	size_t i = 0, j = 0;
	s64 val;
	struct pids_priv *p;

	/* copy the number out of the (not null terminated) write buffer */
	while (i < nbytes && j < sizeof(tmp) - 1 && buf[i] != '\n')
		tmp[j++] = buf[i++];
	tmp[j] = '\0';
	while (j > 0 && (tmp[j - 1] == ' ' || tmp[j - 1] == '\t'))
		tmp[--j] = '\0';

	if (strcmp(tmp, "max") == 0) {
		val = S64_MAX;
	} else {
		char *end;
		long v = simple_strtol(tmp, &end, 10);

		if (end == tmp)
			return -EINVAL;
		while (*end == ' ' || *end == '\t')
			end++;
		if (*end != '\0')
			return -EINVAL;
		val = (s64)v;
		if (val < 0)
			val = 0;
	}

	p = pids_find(cgrp);
	if (!p) {
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		atomic64_set(&p->limit, 0);
		{
			struct pids_priv *old =
				xa_store(&pids_xa, (unsigned long)cgrp, p,
					 GFP_KERNEL);
			if (xa_is_err(old)) {
				kfree(p);
				return xa_err(old);
			}
			if (old) {
				/* lost a race: keep the existing entry */
				kfree(p);
				p = old;
			}
		}
	}
	atomic64_set(&p->limit, val);
	return (ssize_t)nbytes;
}

/*
 * pids.current, as the real controller has it: how many tasks the counter holds
 * for this cgroup. If it stays 0 while a container is forking, the fork probe is
 * not running and the limit cannot be enforced.
 */
static u64 pids_current_read_u64(struct cgroup_subsys_state *css,
				 struct cftype *cft)
{
	struct cgroup *cgrp = css->cgroup;

	/* counted on demand, so this is the true number, never a drifting one */
	struct pids_priv *p = pids_find(cgrp);

	return p ? (u64)atomic64_read(&p->counter) : 0;
}

/*
 * pids.stats: the one number that separates "the probe is not firing" from
 * "the probe fires and lets everything through". subtree counts every write to
 * cgroup.subtree_control that reached us, strip counts the ones where a pids
 * token was removed, fork counts every fork seen, charged the forks where the
 * destination cgroup resolved, refused the ones turned away.
 */
static int pids_stats_show(struct seq_file *sf, void *v)
{
	seq_printf(sf,
		   "subtree=%d strip=%d rewrite=%d fork=%d charged=%d refused=%d "
		   "rmdir=%d layout=%d\n",
		   atomic_read(&ctl_n_subtree), atomic_read(&ctl_n_strip),
		   atomic_read(&ctl_n_rewrite),
		   atomic_read(&ctl_n_fork), atomic_read(&ctl_n_charged),
		   atomic_read(&ctl_n_refused),
		   atomic_read(&ctl_n_rmdir), (int)ctl_layout_ok);
	return 0;
}

static struct cftype pids_max_files[] = {
	{
		.name = "pids.max",
		.flags = CFTYPE_NO_PREFIX | CFTYPE_NOT_ON_ROOT,
		.read_u64 = pids_max_read_u64,
		.write = pids_max_write,
	},
	{
		.name = "pids.current",
		.flags = CFTYPE_NO_PREFIX | CFTYPE_NOT_ON_ROOT,
		.read_u64 = pids_current_read_u64,
	},
	{
		.name = "pids.stats",
		.flags = CFTYPE_NO_PREFIX | CFTYPE_NOT_ON_ROOT,
		.seq_show = pids_stats_show,
	},
	{},
};

/* ---------------------------------------------------------- init/exit ---- */
static struct cgroup_subsys *memory_ss;

static bool ctl_live_subtree;
static bool ctl_live_can_fork;
static bool ctl_live_fork;
static bool ctl_live_exit;
static bool ctl_live_rmdir;
static bool ctl_live_files;

static void ctl_probe_one(struct hk_kprobe *h, const char *sym,
			  kprobe_pre_handler_t pre, bool *live)
{
	unsigned long alt = kallrecon_klp(sym);
	int ret = hk_kprobe_install(h, sym, pre);

	if (ret) {
		pids_ctl_warn("probe %s failed: %d\n", sym, ret);
		return;
	}

	/*
	 * HooKern and the recovery layer can disagree about where a symbol
	 * lives. When they do, rearm on the recovery layer's answer: that is
	 * the lookup that already places cgroup_controllers_show correctly, so
	 * a probe left on HooKern's address would otherwise sit somewhere the
	 * kernel never reaches and look perfectly healthy in the log.
	 */
	if (alt && alt != h->orig) {
		pids_ctl_warn("%s address mismatch: hk=0x%lx kallrecon=0x%lx, "
			      "rearming\n", sym, h->orig, alt);
		hk_kprobe_remove(h);
		ret = hk_kprobe_install_at(h, alt, pre);
		if (ret) {
			pids_ctl_warn("rearm %s at 0x%lx failed: %d\n", sym,
				      alt, ret);
			return;
		}
	}

	*live = true;
	pids_ctl_info("probed %s at 0x%lx\n", sym, h->orig);
}

int droid_lkm_pids_ctl_init(void)
{
	int ret;

	if (!klnum_val || !kallrecon_klp) {
		pids_ctl_err("kallsyms recovery not ready\n");
		return -ENODATA;
	}

	/* the pids.max file hangs off the memory controller, which Droidspaces
	 * always enables for its containers */
	{
		unsigned long mem_ss_addr = kallrecon_klp("memory_cgrp_subsys");

		if (!mem_ss_addr) {
			pids_ctl_err("memory_cgrp_subsys not found\n");
			return -ENODEV;
		}
		memory_ss = (struct cgroup_subsys *)mem_ss_addr;
	}

	ctl_layout_init();

	/* B3 first: without it Droidspaces' subtree_control write fails as a
	 * whole, the container never gets memory, so it never gets pids.max */
	ctl_probe_one(&ctl_subtree_probe, "cgroup_subtree_control_write",
		      ctl_subtree_pre, &ctl_live_subtree);

	/* B2: these dereference css_set and task_struct, so they stay off unless
	 * the runtime layout was verified above */
	if (ctl_layout_ok) {
		ctl_probe_one(&ctl_can_fork_probe, "cgroup_can_fork",
			      ctl_can_fork_pre, &ctl_live_can_fork);
		ctl_probe_one(&ctl_fork_probe, "cgroup_post_fork",
			      ctl_fork_pre, &ctl_live_fork);
		ctl_probe_one(&ctl_exit_probe, "cgroup_exit", ctl_exit_pre,
			      &ctl_live_exit);
		ctl_probe_one(&ctl_rmdir_probe, "cgroup_rmdir", ctl_rmdir_pre,
			      &ctl_live_rmdir);
	} else {
		pids_ctl_warn("fork probe skipped: pids.max is exported "
			      "but limits will not be enforced\n");
	}

	/* B1: expose pids.max. cgroup_add_dfl_cftypes locks cgroup_mutex itself,
	 * so we must NOT hold it here. It returns 0 (and adds nothing) when the
	 * memory controller is disabled, which degrades gracefully. */
	ret = cgroup_add_dfl_cftypes(memory_ss, pids_max_files);
	if (ret)
		pids_ctl_err("cgroup_add_dfl_cftypes failed: %d\n", ret);
	else
		ctl_live_files = true;

	if (ctl_live_can_fork && ctl_live_fork && ctl_live_exit && ctl_live_files)
		pids_ctl_info("pids limit enforcement active "
			      "(emulated pids controller)\n");
	else
		pids_ctl_warn("pids limit enforcement incomplete: "
			      "fork=%d post=%d exit=%d pids.max=%d subtree=%d "
			      "rmdir=%d\n",
			      ctl_live_can_fork, ctl_live_fork, ctl_live_exit,
			      ctl_live_files, ctl_live_subtree, ctl_live_rmdir);

	/* Droidspaces only checks the controller name, so the module stays
	 * loaded even when nothing in this file could be installed */
	return 0;
}

void droid_lkm_pids_ctl_exit(void)
{
	/* cgroup_rm_cftypes locks cgroup_mutex itself */
	if (ctl_live_files)
		cgroup_rm_cftypes(pids_max_files);

	if (ctl_live_rmdir)
		hk_kprobe_remove(&ctl_rmdir_probe);
	if (ctl_live_exit)
		hk_kprobe_remove(&ctl_exit_probe);
	if (ctl_live_fork)
		hk_kprobe_remove(&ctl_fork_probe);
	if (ctl_live_can_fork)
		hk_kprobe_remove(&ctl_can_fork_probe);
	if (ctl_live_subtree)
		hk_kprobe_remove(&ctl_subtree_probe);

	{
		struct pids_priv *p;
		unsigned long idx;

		xa_for_each(&pids_xa, idx, p)
			kfree(p);
		xa_destroy(&pids_xa);
	}
	pids_ctl_info("unloaded\n");
}
