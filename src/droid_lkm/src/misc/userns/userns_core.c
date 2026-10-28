// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/user_namespace.h>

#include "misc.h"
#include "misc_ksym.h"
#include "misc_proc.h"
#include "userns.h"

static LIST_HEAD(droid_lkm_userns_list);
static DEFINE_MUTEX(droid_lkm_userns_lock);

/*
 * real mode. the container gets a uid space of its own: id 0 inside is uid_base on
 * the host, so the task's raw kuid, every inode it creates and every DAC check see
 * a number that is not root
 */
bool droid_lkm_userns_real;

/*
 * one base, shared by every namespace, because the owner of a container's root
 * files is written to disk. a base that moved between two starts of the same
 * container would leave the whole rootfs owned by a uid the container no longer
 * maps, and every read inside it would fail. userns_spread=1 hands each new
 * namespace its own range instead, which is only correct when every container
 * relabels its rootfs on every start
 */
u32 droid_lkm_userns_base = 200000U;
bool droid_lkm_userns_spread;

/*
 * who may ask for a namespace at all. the stock kernel answers EINVAL here, and an
 * application that could hand itself a namespace plus a full capability set would be
 * a door the stock kernel does not have, so the default only serves tasks that are
 * already inside one and tasks that start from root
 */
bool droid_lkm_userns_any;

/*
 * born-in-namespace mode. the container launcher never asks for a user
 * namespace, it only asks for a mount namespace, so the namespace is created
 * at the fork that builds the container and the child is born inside it. the
 * mount namespace that fork creates is then owned by this namespace, which is
 * what makes the mounts the container does next legal instead of borrowed
 */
bool droid_lkm_userns_adopt;

static atomic_t droid_lkm_userns_seq = ATOMIC_INIT(0);

#define DROID_LKM_UID_BASE_FIRST 200000U
#define DROID_LKM_UID_BASE_STEP  70000U
#define DROID_LKM_UID_SPAN       65536U
#define DROID_LKM_UID_SLOTS      8U
#define DROID_LKM_UID_BASE_MIN   10000U

static u32 droid_lkm_userns_next_base(void)
{
	unsigned int idx;

	if (!droid_lkm_userns_spread)
		return droid_lkm_userns_base;

	idx = (unsigned int)atomic_fetch_inc(&droid_lkm_userns_seq);

	return droid_lkm_userns_base +
	       (idx % DROID_LKM_UID_SLOTS) * DROID_LKM_UID_BASE_STEP;
}

bool droid_lkm_userns_is_ours(const struct user_namespace *uns)
{
	struct droid_lkm_userns *u;
	bool found = false;

	if (!uns)
		return false;

	mutex_lock(&droid_lkm_userns_lock);
	list_for_each_entry(u, &droid_lkm_userns_list, link) {
		if (&u->uns == uns) {
			found = true;
			break;
		}
	}
	mutex_unlock(&droid_lkm_userns_lock);

	return found;
}

struct droid_lkm_userns *droid_lkm_userns_of(struct task_struct *task)
{
	struct user_namespace *uns;

	if (!task)
		return NULL;

	uns = __task_cred(task)->user_ns;
	if (!droid_lkm_userns_is_ours(uns))
		return NULL;

	return container_of(uns, struct droid_lkm_userns, uns);
}

/*
 * mirrors create_user_ns(): level, parent, owner, group, flags, empty maps, only the
 * work_struct stays zeroed
 */
struct droid_lkm_userns *droid_lkm_userns_create(struct droid_lkm_userns *parent)
{
	struct droid_lkm_userns *u;
	unsigned int inum;
	int ret, i;

	if (parent && parent->level > 32)
		return NULL;

	u = kzalloc(sizeof(*u), GFP_KERNEL);
	if (!u)
		return NULL;

	ret = droid_lkm_misc_ks.proc_alloc_inum(&inum);
	if (ret) {
		kfree(u);
		return NULL;
	}

	u->level = parent ? parent->level + 1 : 1;
	u->uns.parent = parent ? &parent->uns : &init_user_ns;
	u->uns.level = u->level;
	u->uns.owner = current_euid();
	u->uns.group = current_egid();
	u->uns.flags = (parent ? parent->uns.flags : USERNS_INIT_FLAGS);
	u->uns.parent_could_setfcap = cap_raised(current_cred()->cap_effective,
						 CAP_SETFCAP);
	u->uns.ns.inum = inum;
	u->uns.ns.ops = droid_lkm_userns_ops();
	refcount_set(&u->uns.ns.count, 1);
	u->uns.ucounts = NULL;
	/*
	 * inc_ucount() walks ns->ucount_max[], which zero would turn into ENOSPC for every
	 * copy_utsname(), copy_mnt_ns() and copy_net_ns(), so mirror create_user_ns(). ucounts stays
	 * NULL, nothing is accounted per user
	 */
	for (i = 0; i < UCOUNT_COUNTS; i++)
		u->uns.ucount_max[i] = INT_MAX;
	for (i = 0; i < UCOUNT_RLIMIT_COUNTS; i++)
		u->uns.rlimit_max[i] = LONG_MAX;
	INIT_LIST_HEAD(&u->link);

	/*
	 * a namespace nested inside a real one inherits the base, so the ids of a
	 * container that creates a second namespace keep pointing at the same owner.
	 * anything else takes a fresh base when real mode is on
	 */
	if (parent && parent->real) {
		u->real = true;
		u->uid_base = parent->uid_base;
		u->gid_base = parent->gid_base;
		droid_lkm_userns_map_set(u, 0, u->uid_base, DROID_LKM_UID_SPAN);
	} else if (droid_lkm_userns_real) {
		u->real = true;
		u->uid_base = droid_lkm_userns_next_base();
		u->gid_base = u->uid_base;
		droid_lkm_userns_map_set(u, 0, u->uid_base, DROID_LKM_UID_SPAN);
	} else {
		/* the identity map, the only one a container on this kernel could write */
		droid_lkm_userns_map_identity(u);
	}

	mutex_lock(&droid_lkm_userns_lock);
	list_add_tail(&u->link, &droid_lkm_userns_list);
	mutex_unlock(&droid_lkm_userns_lock);

	droid_lkm_userns_view_arm(u);

	droid_lkm_misc_dbg("userns create %p level=%u inum=%u owner=%u real=%d base=%u\n",
		u, u->level, inum, __kuid_val(u->uns.owner), u->real, u->uid_base);

	return u;
}

void droid_lkm_userns_destroy(struct droid_lkm_userns *u)
{
	if (!u)
		return;

	/* the syscall layer looks this up without a lock, so take it out of reach first */
	droid_lkm_userns_view_disarm(u);

	mutex_lock(&droid_lkm_userns_lock);
	list_del_init(&u->link);
	mutex_unlock(&droid_lkm_userns_lock);

	droid_lkm_misc_ks.proc_free_inum(u->uns.ns.inum);
	kfree(u);
}

int droid_lkm_userns_init(void)
{
	char detail[32];
	int ret;

	/*
	 * a base below the first application uid would hand a container the identity of
	 * an installed app, and a base this close to the top of the range would run off
	 * the end of the 32 bit id space. both fall back to the default instead of
	 * refusing to load, so a typo costs a log line and not the module
	 */
	if (droid_lkm_userns_base < DROID_LKM_UID_BASE_MIN ||
	    droid_lkm_userns_base > 4294967294U - DROID_LKM_UID_SPAN) {
		droid_lkm_misc_warn("userns base %u out of range, using %u\n",
				    droid_lkm_userns_base, DROID_LKM_UID_BASE_FIRST);
		droid_lkm_userns_base = DROID_LKM_UID_BASE_FIRST;
	}

	ret = droid_lkm_userns_nsops_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "namespace entry failed");
		return ret;
	}

	ret = droid_lkm_userns_owner_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "owner symbols missing");
		droid_lkm_userns_nsops_exit();
		return ret;
	}

	ret = droid_lkm_userns_hooks_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "a hook failed");
		droid_lkm_userns_owner_exit();
		droid_lkm_userns_nsops_exit();
		return ret;
	}

	droid_lkm_userns_map_selftest();

	/*
	 * the syscall translation layer is what makes a real map usable: without it the
	 * container would read the outer uid back and every program that checks for root
	 * would refuse to run. it is only armed when real mode asked for it
	 */
	ret = droid_lkm_userns_view_init();
	if (ret) {
		droid_lkm_misc_warn("userns view unavailable: %d\n", ret);
		droid_lkm_userns_real = false;
	}

	ret = droid_lkm_userns_captrace_init();
	if (ret)
		droid_lkm_misc_warn("cap trace unavailable: %d\n", ret);

	/* the map files are an addition to the namespace, a kernel without them keeps the rest */
	ret = droid_lkm_userns_proc_init();
	if (ret)
		droid_lkm_misc_warn("id map files unavailable: %d\n", ret);

	droid_lkm_misc_report("userns", "ready",
			      droid_lkm_userns_real ?
			      "unshare clone namespace entry credential swap, real uid map" :
			      "unshare clone namespace entry credential swap");
	/* the relabel helper reads the base from here, so it never hardcodes one */
	scnprintf(detail, sizeof(detail), "base=%u spread=%d",
		  droid_lkm_userns_base, droid_lkm_userns_spread);
	droid_lkm_misc_report("userns base", "ready", detail);
	droid_lkm_misc_report("userns cap trace", droid_lkm_userns_captrace_on ?
			      "on" : "off", "captrace=<bool>");
	droid_lkm_misc_info("userns ready\n");
	droid_lkm_userns_selftest_run();

	/* reported after the sample, so the count includes the namespace it left behind */
	scnprintf(detail, sizeof(detail), "%u live", droid_lkm_userns_live());
	droid_lkm_misc_report("userns objects", "ready", detail);

	return 0;
}

/*
 * 被 task 的 cred 引用的对象数。只有这种对象才让模块卸不掉：卸载后
 * cred->user_ns 指着模块内存，下一次 ns_capable() 就会踩进去
 */
unsigned int droid_lkm_userns_busy(void)
{
	struct droid_lkm_userns *u;
	unsigned int count = 0;

	mutex_lock(&droid_lkm_userns_lock);
	list_for_each_entry(u, &droid_lkm_userns_list, link) {
		if (refcount_read(&u->uns.ns.count) > 1)
			count++;
	}
	mutex_unlock(&droid_lkm_userns_lock);

	return count;
}

/*
 * find the namespace this module built for a given user_namespace pointer. the
 * mount namespace keeps a reference to it, so a launcher that already unshared
 * can be recognised from that side even though its own credential is still the
 * host root's
 */
struct droid_lkm_userns *droid_lkm_userns_by_ns(const struct user_namespace *uns)
{
	struct droid_lkm_userns *u, *found = NULL;

	if (!uns)
		return NULL;

	mutex_lock(&droid_lkm_userns_lock);
	list_for_each_entry(u, &droid_lkm_userns_list, link) {
		if (&u->uns == uns) {
			found = u;
			break;
		}
	}
	mutex_unlock(&droid_lkm_userns_lock);

	return found;
}

unsigned int droid_lkm_userns_live(void)
{
	struct droid_lkm_userns *u;
	unsigned int count = 0;

	mutex_lock(&droid_lkm_userns_lock);
	list_for_each_entry(u, &droid_lkm_userns_list, link)
		count++;
	mutex_unlock(&droid_lkm_userns_lock);

	return count;
}

void droid_lkm_userns_exit(void)
{
	struct droid_lkm_userns *u, *tmp;

	droid_lkm_userns_proc_exit();
	droid_lkm_userns_captrace_exit();
	/* the probes come off before any namespace object can be freed */
	droid_lkm_userns_view_exit();
	droid_lkm_userns_hooks_exit();
	droid_lkm_userns_owner_exit();

	mutex_lock(&droid_lkm_userns_lock);
	list_for_each_entry_safe(u, tmp, &droid_lkm_userns_list, link) {
		list_del_init(&u->link);

		/*
		 * count 初始就是 1，>1 说明还有 task 的 cred 指着这个 ns。
		 * 这时候 free 掉，卸载后 cred->user_ns 就是悬空指针，下一次
		 * 能力检查（ns_capable）直接 use-after-free。宁可漏这点内存，
		 * 也不能把整机交待在这里
		 */
		if (refcount_read(&u->uns.ns.count) > 1) {
			droid_lkm_misc_warn("userns: inum %u still referenced (%d), left in place\n",
					    u->uns.ns.inum,
					    refcount_read(&u->uns.ns.count));
			continue;
		}

		droid_lkm_misc_ks.proc_free_inum(u->uns.ns.inum);
		kfree(u);
	}
	mutex_unlock(&droid_lkm_userns_lock);

	droid_lkm_userns_nsops_exit();
}
