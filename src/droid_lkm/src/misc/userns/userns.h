// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef DROID_LKM_MISC_USERNS_H
#define DROID_LKM_MISC_USERNS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/uidgid.h>
#include <linux/ns_common.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>

struct task_struct;
struct cred;

/*
 * one fake user namespace. struct user_namespace comes first so the object can be handed to
 * the kernel and found again from a struct user_namespace pointer. the map starts empty like a
 * fresh namespace and becomes the identity map when written, an empty map reports 65534
 */
struct droid_lkm_userns {
	struct user_namespace uns;
	struct list_head link;
	unsigned int level;
	bool map_written;

	/*
	 * real mode only: the host side of this container's uid space. every task in
	 * the namespace carries uid_base + <id inside the container> as its raw kuid,
	 * so DAC, ps and the filesystem work on the real number while the syscall
	 * layer in userns_view.c turns it back into the number the container reads.
	 */
	bool real;
	u32 uid_base;
	u32 gid_base;
};

int droid_lkm_userns_init(void);
void droid_lkm_userns_exit(void);

int droid_lkm_userns_hooks_init(void);
void droid_lkm_userns_hooks_exit(void);

/* the initial user namespace, resolved once and shared by the owner work */
extern struct user_namespace *droid_lkm_userns_init_uns;

int droid_lkm_userns_owner_init(void);
void droid_lkm_userns_owner_exit(void);

extern bool droid_lkm_userns_captrace_on;
int droid_lkm_userns_captrace_init(void);
void droid_lkm_userns_captrace_exit(void);

/* /proc/<pid>/{uid_map,gid_map,setgroups}, added back under the pid directory */
int droid_lkm_userns_proc_init(void);
void droid_lkm_userns_proc_exit(void);

/* layers two and three of the install time self test */
void droid_lkm_userns_selftest_run(void);

struct droid_lkm_userns *droid_lkm_userns_create(struct droid_lkm_userns *parent);
void droid_lkm_userns_destroy(struct droid_lkm_userns *u);
struct droid_lkm_userns *droid_lkm_userns_of(struct task_struct *task);
struct droid_lkm_userns *droid_lkm_userns_by_ns(const struct user_namespace *uns);
unsigned int droid_lkm_userns_live(void);
unsigned int droid_lkm_userns_busy(void);
bool droid_lkm_userns_is_ours(const struct user_namespace *uns);

int droid_lkm_userns_install_current(struct droid_lkm_userns *u);
void droid_lkm_userns_install_cred(struct cred *new, struct droid_lkm_userns *u);

const struct proc_ns_operations *droid_lkm_userns_ops(void);
const struct proc_ns_operations *droid_lkm_userns_ops_for(struct task_struct *task);
int droid_lkm_userns_nsops_init(void);
void droid_lkm_userns_nsops_exit(void);

u32 droid_lkm_userns_map_down(const struct droid_lkm_userns *u, u32 id);
u32 droid_lkm_userns_map_up(const struct droid_lkm_userns *u, u32 id);
bool droid_lkm_userns_map_ok(const struct droid_lkm_userns *u, u32 id);
void droid_lkm_userns_map_identity(struct droid_lkm_userns *u);
void droid_lkm_userns_map_set(struct droid_lkm_userns *u, u32 inner, u32 outer,
			      u32 count);
void droid_lkm_userns_map_selftest(void);

/* the user visible side: syscalls translate between the two id spaces */
extern bool droid_lkm_userns_real;

/* host side id of container id 0; one value for every namespace unless spread is on */
extern u32 droid_lkm_userns_base;
extern bool droid_lkm_userns_spread;
extern bool droid_lkm_userns_any;
extern bool droid_lkm_userns_adopt;
int droid_lkm_userns_view_init(void);
void droid_lkm_userns_view_exit(void);
void droid_lkm_userns_view_arm(struct droid_lkm_userns *u);
void droid_lkm_userns_view_disarm(struct droid_lkm_userns *u);

#endif
