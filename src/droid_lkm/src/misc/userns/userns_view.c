// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * The user visible side of a real user namespace.
 *
 * This kernel was built without CONFIG_USER_NS, so from_kuid() and make_kuid()
 * are inline identities: the number a syscall returns is the raw kuid stored in
 * the credential, and the number a syscall stores is the raw kuid that lands in
 * the inode. A mapping therefore cannot be attached to the conversion helpers -
 * there are none to attach it to. It is attached to the calls instead, which is
 * the only place a container and the host exchange an id:
 *
 *   getuid geteuid getgid getegid      raw id -> container id   (return value)
 *   set*uid set*gid setfs*id           container id -> raw id   (argument)
 *   chown lchown fchown fchownat       container id -> raw id   (argument)
 *
 * The raw id is the real one. It is what ps and ls print on the host, what the
 * filesystem stores, and what every DAC comparison uses. Only the number the
 * container's own processes read back is rewritten, which is exactly what a
 * mapping does on a kernel that has one.
 *
 * An id outside the map is refused the way upstream refuses it, with an error
 * rather than a silent pass: letting setuid(0) through unmapped would hand the
 * container the host's root, which is the whole thing this layer exists to stop.
 *
 * Not covered here and not claimed to be: getresuid() and getresgid() write
 * through user pointers, which a probe cannot do without faulting, and stat
 * output still prints the host id. Both are display, not privilege.
 */

#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/stat.h>

#include "misc.h"
#include "misc_proc.h"
#include "userns.h"
#include "hk_kprobe.h"

#define DROID_LKM_VIEW_SLOTS 16
#define DROID_LKM_VIEW_NOBODY 65534U

/*
 * armed namespaces, read on every getuid of every process on the phone. no lock
 * on the read: a namespace only leaves this table from droid_lkm_userns_destroy(),
 * which runs before the object is freed and only on a path where no credential
 * points at it yet
 */
static struct droid_lkm_userns *droid_lkm_view_slots[DROID_LKM_VIEW_SLOTS];
static DEFINE_SPINLOCK(droid_lkm_view_lock);

static atomic64_t droid_lkm_view_nread;
static bool droid_lkm_view_up;
static atomic64_t droid_lkm_view_nwrite;
static atomic64_t droid_lkm_view_ndeny;

void droid_lkm_userns_view_arm(struct droid_lkm_userns *u)
{
	unsigned int i;

	/*
	 * every namespace is registered, identity or real. the identity map translates
	 * an id to itself, so one code path serves both and there is no mode in which a
	 * namespace exists but the layer does not know it
	 */
	if (!u)
		return;

	spin_lock(&droid_lkm_view_lock);
	for (i = 0; i < DROID_LKM_VIEW_SLOTS; i++) {
		if (!droid_lkm_view_slots[i]) {
			WRITE_ONCE(droid_lkm_view_slots[i], u);
			break;
		}
	}
	spin_unlock(&droid_lkm_view_lock);
}

void droid_lkm_userns_view_disarm(struct droid_lkm_userns *u)
{
	unsigned int i;

	if (!u)
		return;

	spin_lock(&droid_lkm_view_lock);
	for (i = 0; i < DROID_LKM_VIEW_SLOTS; i++) {
		if (droid_lkm_view_slots[i] == u) {
			WRITE_ONCE(droid_lkm_view_slots[i], NULL);
			break;
		}
	}
	spin_unlock(&droid_lkm_view_lock);
}

static struct droid_lkm_userns *droid_lkm_view_current(void)
{
	struct user_namespace *uns = current_cred()->user_ns;
	unsigned int i;

	if (!uns)
		return NULL;

	for (i = 0; i < DROID_LKM_VIEW_SLOTS; i++) {
		struct droid_lkm_userns *u = READ_ONCE(droid_lkm_view_slots[i]);

		if (u && &u->uns == uns)
			return u;
	}

	return NULL;
}

/* ------------------------------------------------------------- reads ---- */
/*
 * getuid() and its three siblings are answered at their entry: the handler fills
 * x0 with the translated id and moves the pc to the link register, so the body
 * never runs and the caller reads the id we chose. a kretprobe would work too and
 * is what this did first, but it depends on the return instance being available,
 * and a missed instance is silent - an entry probe either arms or fails loudly.
 *
 * nothing is read out of the registers at entry, the id is taken from the
 * credential the function itself would have read, so all four calls share one
 * handler and differ only in which field they name
 */
enum {
	DROID_LKM_VIEW_UID,
	DROID_LKM_VIEW_EUID,
	DROID_LKM_VIEW_GID,
	DROID_LKM_VIEW_EGID,
};

struct droid_lkm_view_read {
	struct hk_kprobe probe;
	const char *name;
	unsigned int which;
};

static struct droid_lkm_view_read droid_lkm_view_reads[] = {
	{ .name = "__arm64_sys_getuid",  .which = DROID_LKM_VIEW_UID },
	{ .name = "__arm64_sys_geteuid", .which = DROID_LKM_VIEW_EUID },
	{ .name = "__arm64_sys_getgid",  .which = DROID_LKM_VIEW_GID },
	{ .name = "__arm64_sys_getegid", .which = DROID_LKM_VIEW_EGID },
};

/* printed once, so a load log says whether the layer is live without a repro */
static atomic_t droid_lkm_view_first = ATOMIC_INIT(0);

static void droid_lkm_view_note(u32 raw, u32 vis)
{
	if (atomic_cmpxchg(&droid_lkm_view_first, 0, 1) == 0)
		droid_lkm_misc_info("userns view: live, raw %u reads as %u\n",
				    raw, vis);
}

static int droid_lkm_view_read_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct droid_lkm_view_read *r =
		container_of(container_of(p, struct hk_kprobe, kp),
			     struct droid_lkm_view_read, probe);
	struct droid_lkm_userns *u = droid_lkm_view_current();
	const struct cred *c;
	u32 raw, vis;

	if (!u)
		return 0;

	c = current_cred();
	switch (r->which) {
	case DROID_LKM_VIEW_UID:
		raw = __kuid_val(c->uid);
		break;
	case DROID_LKM_VIEW_EUID:
		raw = __kuid_val(c->euid);
		break;
	case DROID_LKM_VIEW_GID:
		raw = __kgid_val(c->gid);
		break;
	default:
		raw = __kgid_val(c->egid);
		break;
	}

	vis = droid_lkm_userns_map_up(u, raw);
	if (vis == (u32)-1)
		vis = DROID_LKM_VIEW_NOBODY;

	hk_regs_set_ret(regs, vis);
	hk_regs_return(regs);
	atomic64_inc(&droid_lkm_view_nread);
	droid_lkm_view_note(raw, vis);

	return 1;
}

/* ------------------------------------------------------- stat owner ---- */
/*
 * stat and friends read the inode's raw kuid straight into struct kstat, and the
 * conversion that would translate it is compiled out. rather than translate the
 * user buffer after the fact - which needs a copy_to_user in a context that may
 * not fault - the value is translated before the kernel formats it.
 *
 * the hook sits on vfs_getattr() rather than on cp_new_stat() or cp_statx():
 * those two are local symbols, present in kallsyms only when the kernel is built
 * with CONFIG_KALLSYMS_ALL, and on this device one of them is not. vfs_getattr is
 * exported, fills the kstat for every stat, lstat, fstat, fstatat and statx, and
 * takes the struct kstat as its second argument
 */
static atomic_t droid_lkm_view_stat_seen = ATOMIC_INIT(0);

static int droid_lkm_view_stat_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct droid_lkm_userns *u = droid_lkm_view_current();
	struct kstat *st = (struct kstat *)hk_regs_arg(regs, 1);
	u32 vis;

	if (!u || !st)
		return 0;

	vis = droid_lkm_userns_map_up(u, __kuid_val(st->uid));
	st->uid = KUIDT_INIT(vis == (u32)-1 ? DROID_LKM_VIEW_NOBODY : vis);

	vis = droid_lkm_userns_map_up(u, __kgid_val(st->gid));
	st->gid = KGIDT_INIT(vis == (u32)-1 ? DROID_LKM_VIEW_NOBODY : vis);

	if (atomic_cmpxchg(&droid_lkm_view_stat_seen, 0, 1) == 0)
		droid_lkm_misc_info("userns view: stat owner translated\n");

	return 0;
}

static struct hk_kprobe droid_lkm_view_getattr;

/* ------------------------------------------------------------ writes ---- */
/*
 * one entry per call that carries an id inward. slot[] names the argument
 * registers, n says how many of them belong to the call. every call in this
 * family spells "do not change this one" as (uid_t)-1, which is passed through
 * untouched
 */
struct droid_lkm_view_arg {
	struct hk_kprobe probe;
	const char *name;
	unsigned int n;
	unsigned int slot[3];
};

static struct droid_lkm_view_arg droid_lkm_view_args[] = {
	{ .name = "__arm64_sys_setuid",    .n = 1, .slot = { 0 } },
	{ .name = "__arm64_sys_setgid",    .n = 1, .slot = { 0 } },
	{ .name = "__arm64_sys_setfsuid",  .n = 1, .slot = { 0 } },
	{ .name = "__arm64_sys_setfsgid",  .n = 1, .slot = { 0 } },
	{ .name = "__arm64_sys_setreuid",  .n = 2, .slot = { 0, 1 } },
	{ .name = "__arm64_sys_setregid",  .n = 2, .slot = { 0, 1 } },
	{ .name = "__arm64_sys_setresuid", .n = 3, .slot = { 0, 1, 2 } },
	{ .name = "__arm64_sys_setresgid", .n = 3, .slot = { 0, 1, 2 } },
	{ .name = "__arm64_sys_chown",     .n = 2, .slot = { 1, 2 } },
	{ .name = "__arm64_sys_lchown",    .n = 2, .slot = { 1, 2 } },
	{ .name = "__arm64_sys_fchown",    .n = 2, .slot = { 1, 2 } },
	{ .name = "__arm64_sys_fchownat",  .n = 2, .slot = { 2, 3 } },
};

/*
 * setgroups() carries a user pointer, so there is no register to translate: the
 * gid list would land in the credential raw, which is a way around the map for
 * exactly the group that matters. upstream refuses it until the map is written
 * and setgroups is switched off for good reason. a container here never needs it,
 * so it is refused outright once the task is inside one of these namespaces
 */
static int droid_lkm_view_setgroups_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (!droid_lkm_view_current())
		return 0;

	hk_regs_set_ret(regs, (unsigned long)-EPERM);
	hk_regs_return(regs);
	atomic64_inc(&droid_lkm_view_ndeny);

	return 1;
}

static struct hk_kprobe droid_lkm_view_setgroups;

static int droid_lkm_view_arg_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct droid_lkm_view_arg *a =
		container_of(container_of(p, struct hk_kprobe, kp),
			     struct droid_lkm_view_arg, probe);
	struct droid_lkm_userns *u = droid_lkm_view_current();
	unsigned int i;

	if (!u)
		return 0;

	for (i = 0; i < a->n; i++) {
		unsigned int s = a->slot[i];
		u32 id = (u32)hk_regs_arg(regs, s);
		u32 raw;

		if (id == (u32)-1)
			continue;

		raw = droid_lkm_userns_map_down(u, id);
		if (raw == (u32)-1) {
			/*
			 * outside the map. the kernel would take it anyway, because a
			 * task in this namespace holds CAP_SETUID inside it, and the raw
			 * number would become the host id it names
			 */
			atomic64_inc(&droid_lkm_view_ndeny);
			hk_regs_set_ret(regs, (unsigned long)-EPERM);
			hk_regs_return(regs);
			return 1;
		}

		regs->regs[s] = raw;
	}

	atomic64_inc(&droid_lkm_view_nwrite);

	return 0;
}

/* ------------------------------------------------------------- setup ---- */

/* so the status line names what is missing instead of only counting it */
static char droid_lkm_view_missing[56];

static void droid_lkm_view_add_missing(const char *name)
{
	size_t len = strlen(droid_lkm_view_missing);

	if (len && len + 1 < sizeof(droid_lkm_view_missing))
		strlcat(droid_lkm_view_missing, ",", sizeof(droid_lkm_view_missing));
	strlcat(droid_lkm_view_missing, name, sizeof(droid_lkm_view_missing));
}

int droid_lkm_userns_view_init(void)
{
	unsigned long ok = 0, miss = 0;

	if (droid_lkm_view_up)
		return 0;
	unsigned int i;

	/*
	 * armed unconditionally. the handlers are no ops for a task outside one of
	 * these namespaces, and a probe that is only installed on one code path is a
	 * probe that silently is not installed on another
	 */
	for (i = 0; i < ARRAY_SIZE(droid_lkm_view_reads); i++) {
		struct droid_lkm_view_read *r = &droid_lkm_view_reads[i];

		if (hk_kprobe_install(&r->probe, r->name, droid_lkm_view_read_pre)) {
			droid_lkm_misc_warn("userns view: %s unavailable\n", r->name);
			droid_lkm_view_add_missing(r->name);
			miss++;
			continue;
		}
		ok++;
	}

	/*
	 * without the read side the container would see the raw id and refuse to run
	 * as root, so real mode cannot stand on its own without at least one of them
	 */
	if (!ok && droid_lkm_userns_real) {
		droid_lkm_misc_err("userns view: no id call could be probed\n");
		return -ENODATA;
	}

	for (i = 0; i < ARRAY_SIZE(droid_lkm_view_args); i++) {
		struct droid_lkm_view_arg *a = &droid_lkm_view_args[i];

		if (hk_kprobe_install(&a->probe, a->name, droid_lkm_view_arg_pre)) {
			droid_lkm_misc_dbg("userns view: %s unavailable\n", a->name);
			droid_lkm_view_add_missing(a->name);
			miss++;
			continue;
		}
		ok++;
	}

	/* display only: a miss here costs nothing but a raw owner in ls -l */
	if (hk_kprobe_install(&droid_lkm_view_getattr, "vfs_getattr",
			      droid_lkm_view_stat_pre)) {
		droid_lkm_view_add_missing("vfs_getattr");
		miss++;
	} else {
		ok++;
	}

	if (hk_kprobe_install(&droid_lkm_view_setgroups, "__arm64_sys_setgroups",
			      droid_lkm_view_setgroups_pre)) {
		droid_lkm_view_add_missing("__arm64_sys_setgroups");
		miss++;
	} else {
		ok++;
	}

	droid_lkm_view_up = true;

	droid_lkm_misc_info("userns view: %lu armed, %lu unavailable, real=%d\n",
			    ok, miss, droid_lkm_userns_real);

	/*
	 * also in the status file: dmesg is a ring buffer that rotates, this is not,
	 * and a container that reads 270000 instead of 0 is diagnosed in one cat
	 */
	{
		char detail[72];

		if (miss)
			scnprintf(detail, sizeof(detail), "%lu armed, missing %s",
				  ok, droid_lkm_view_missing);
		else
			scnprintf(detail, sizeof(detail), "%lu armed 0 missing real=%d",
				  ok, droid_lkm_userns_real);
		droid_lkm_misc_report("userns view", miss ? "degraded" : "armed", detail);
	}

	return 0;
}

bool droid_lkm_userns_view_armed(void)
{
	return droid_lkm_view_up;
}

void droid_lkm_userns_view_exit(void)
{
	unsigned int i;

	/*
	 * called twice on purpose: once before rmmod (release=1 drops the probe on
	 * the hottest syscall while the module is still mapped), once from module
	 * exit. unregister_kprobe on an already dropped probe is not a no-op, so
	 * the second pass has to return here
	 */
	if (!droid_lkm_view_up)
		return;
	droid_lkm_view_up = false;

	for (i = 0; i < ARRAY_SIZE(droid_lkm_view_reads); i++)
		hk_kprobe_remove(&droid_lkm_view_reads[i].probe);

	hk_kprobe_remove(&droid_lkm_view_setgroups);
	hk_kprobe_remove(&droid_lkm_view_getattr);

	for (i = 0; i < ARRAY_SIZE(droid_lkm_view_args); i++)
		hk_kprobe_remove(&droid_lkm_view_args[i].probe);

	spin_lock(&droid_lkm_view_lock);
	memset(droid_lkm_view_slots, 0, sizeof(droid_lkm_view_slots));
	spin_unlock(&droid_lkm_view_lock);

	droid_lkm_misc_info("userns view: %lld reads, %lld writes, %lld refused\n",
			    (long long)atomic64_read(&droid_lkm_view_nread),
			    (long long)atomic64_read(&droid_lkm_view_nwrite),
			    (long long)atomic64_read(&droid_lkm_view_ndeny));
}
