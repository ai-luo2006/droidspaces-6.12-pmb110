// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * The chokepoints that cannot be answered by correcting state.
 *
 * ksys_unshare and copy_creds are where a request for CLONE_NEWUSER is turned
 * into a fake namespace. The flag is stripped before the kernel sees it, the
 * kernel keeps handling the other namespaces, and the credential side is
 * installed afterwards: on current for unshare, on the child for clone, which
 * cannot have run yet at that point.
 *
 * unshare_nsproxy_namespaces and capable_wrt_inode_uidgid are the two decisions
 * that read current_user_ns(), which this kernel compiles to a constant, so
 * there is no state to correct: the question itself names the host. Both keep
 * the original as the fast path and only add the container case. The owners the
 * kernel fills in through the get_user_ns() stub are corrected in
 * userns_owner.c, which is why may_mount() and mount_capable() have no hook:
 * may_mount() is inlined into path_mount() and could not be hooked anyway.
 */

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/nsproxy.h>
#include <linux/utsname.h>
#include <linux/cgroup.h>
#include <linux/time_namespace.h>
#include <net/net_namespace.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mnt_idmapping.h>
#include <linux/err.h>
#include <linux/uidgid.h>
#include <linux/string.h>
#include <linux/sched/task.h>

#include "misc.h"
#include "misc_ksym.h"
#include "userns.h"
#include "hk_inline.h"

static struct hk_inline droid_lkm_userns_unshare_hook;
static struct hk_inline droid_lkm_userns_nsproxy_hook;
static struct hk_inline droid_lkm_userns_creds_hook;
static struct hk_inline droid_lkm_userns_inodecap_hook;
static struct hk_inline droid_lkm_userns_kclone_hook;
static struct hk_inline droid_lkm_userns_exec_hook;

static int (*droid_lkm_userns_unshare_orig)(unsigned long flags);
static int (*droid_lkm_userns_nsproxy_orig)(unsigned long unshare_flags,
					    struct nsproxy **new_nsp,
					    struct cred *new_cred,
					    struct fs_struct *new_fs);
static int (*droid_lkm_userns_creds_orig)(struct task_struct *p,
					  unsigned long clone_flags);
static bool (*droid_lkm_userns_inodecap_orig)(struct mnt_idmap *idmap,
					      const struct inode *inode, int cap);

/*
 * unshare_nsproxy_namespaces() picks current_user_ns(), the initial namespace here, and the
 * capability test then fails for a task in a fake one. build them with the task's own
 * namespace as owner, through create_new_namespaces() so every constructor runs
 */
__nocfi noinline int droid_lkm_userns_nsproxy_wrap(unsigned long unshare_flags,
						   struct nsproxy **new_nsp,
						   struct cred *new_cred,
						   struct fs_struct *new_fs)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);
	struct nsproxy *nsp;
	int ret;

	if (!u || new_cred || !cap_raised(current_cred()->cap_effective, CAP_SYS_ADMIN))
		return droid_lkm_userns_nsproxy_orig(unshare_flags, new_nsp, new_cred, new_fs);

	nsp = droid_lkm_misc_ks.create_new_namespaces(unshare_flags, current, &u->uns,
						      new_fs ? new_fs : current->fs);
	if (IS_ERR(nsp))
		return PTR_ERR(nsp);

	/*
	 * get_user_ns() is a stub returning the initial namespace, so the constructors stored the
	 * wrong owner. only namespaces this call created are touched, the mount namespace is covered
	 * by may_mount()
	 */
	if (unshare_flags & CLONE_NEWUTS)
		nsp->uts_ns->user_ns = &u->uns;
	if (unshare_flags & CLONE_NEWNET)
		nsp->net_ns->user_ns = &u->uns;
	if (unshare_flags & CLONE_NEWCGROUP)
		nsp->cgroup_ns->user_ns = &u->uns;
	if ((unshare_flags & CLONE_NEWTIME) && nsp->time_ns_for_children)
		nsp->time_ns_for_children->user_ns = &u->uns;

	*new_nsp = nsp;
	ret = 0;

	return ret;
}

/*
 * the first calls are logged so a phone-side report can tell "never asked" from
 * "asked and refused" without another build
 */
static atomic_t droid_lkm_userns_log_budget = ATOMIC_INIT(16);

static bool droid_lkm_userns_log_this(void)
{
	return atomic_dec_if_positive(&droid_lkm_userns_log_budget) >= 0;
}

/*
 * the namespace the container was born into. only one is tracked: every container
 * this module builds shares the same id range (userns_base), so a second one adds
 * nothing a second pointer would find
 */
static struct droid_lkm_userns *droid_lkm_userns_adopted;

/*
 * the mount namespace that was built for it. kept as a pointer only: the layout of
 * struct mnt_namespace is not part of the headers a module is allowed to see, and
 * comparing the pointer is all that is needed to tell "a process inside the
 * container" from "a process on the host"
 */
static void *droid_lkm_userns_adopted_mnt;

/*
 * the launcher runs as root, has not entered a namespace yet, and is about to
 * create the mount namespace the container will live in. nothing else on the
 * phone matches all four, which is what keeps an unrelated root process from
 * being touched
 */
static bool droid_lkm_userns_adopt_unshare(unsigned long flags)
{
	return droid_lkm_userns_adopt && droid_lkm_userns_real &&
	       (flags & CLONE_NEWNS) && !(flags & CLONE_NEWUSER) &&
	       !droid_lkm_userns_of(current) &&
	       uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
	       !strncmp(current->comm, "droidspaces", 11);
}

static pid_t (*droid_lkm_userns_kclone_orig)(struct kernel_clone_args *args);
static int (*droid_lkm_userns_exec_orig)(struct filename *filename,
					 const char __user *const __user *argv,
					 const char __user *const __user *envp);

/* a task that shares the container's mount namespace but still holds host root */
static bool droid_lkm_userns_needs_adopt(void)
{
	if (!droid_lkm_userns_adopted_mnt || !current->nsproxy)
		return false;
	if ((void *)current->nsproxy->mnt_ns != droid_lkm_userns_adopted_mnt)
		return false;
	if (droid_lkm_userns_of(current))
		return false;

	return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

/*
 * born-in-namespace, step two. every process the launcher forks after the mount
 * namespace exists is born into the container: the credential is swapped for the
 * length of the fork, so the child (whose credential is a copy made inside
 * copy_process) inherits the container's, and the parent is put back before it
 * returns. This is the same trick as the unshare case, applied one level down
 */
__nocfi noinline pid_t droid_lkm_userns_kclone_wrap(struct kernel_clone_args *args)
{
	struct cred *old;
	pid_t ret;

	if (!droid_lkm_userns_needs_adopt())
		return droid_lkm_userns_kclone_orig(args);

	old = (struct cred *)get_cred(current->cred);
	if (droid_lkm_userns_install_current(droid_lkm_userns_adopted)) {
		put_cred(old);
		return droid_lkm_userns_kclone_orig(args);
	}

	ret = droid_lkm_userns_kclone_orig(args);

	droid_lkm_misc_ks.commit_creds(old);
	droid_lkm_misc_info("adopt: clone %s pid=%d -> child %d\n",
			    current->comm, current->pid, ret);

	return ret;
}

/*
 * last resort for a container that is exec'd in place instead of forked. the
 * credential is swapped before the image is loaded, so the program starts in the
 * container's id space and everything it opens afterwards is checked against it
 */
__nocfi noinline int droid_lkm_userns_exec_wrap(struct filename *filename,
						const char __user *const __user *argv,
						const char __user *const __user *envp)
{
	if (droid_lkm_userns_needs_adopt() &&
	    !droid_lkm_userns_install_current(droid_lkm_userns_adopted))
		droid_lkm_misc_info("adopt: exec %s pid=%d entered ns\n",
				    current->comm, current->pid);

	return droid_lkm_userns_exec_orig(filename, argv, envp);
}

__nocfi noinline int droid_lkm_userns_unshare_wrap(unsigned long flags)
{
	struct droid_lkm_userns *u;
	unsigned long rest;
	int ret;

	/*
	 * born-in-namespace, step one. A mount namespace is owned by the user
	 * namespace the caller's credential names at the moment it is created (see
	 * create_new_namespaces): every mount made in it later is checked against that
	 * owner. So the credential is swapped for exactly the length of this one call,
	 * and the caller gets its host rights back before the syscall returns. What is
	 * left behind is a mount namespace that legitimately belongs to the container
	 */
	if (droid_lkm_userns_adopt_unshare(flags)) {
		struct cred *old = (struct cred *)get_cred(current->cred);

		u = droid_lkm_userns_create(NULL);
		if (!u) {
			put_cred(old);
			return -ENOMEM;
		}

		if (droid_lkm_userns_install_current(u)) {
			put_cred(old);
			droid_lkm_userns_destroy(u);
			return -ENOMEM;
		}

		ret = droid_lkm_userns_unshare_orig(flags);

		droid_lkm_misc_ks.commit_creds(old);

		if (ret) {
			droid_lkm_userns_destroy(u);
		} else {
			droid_lkm_userns_adopted = u;
			droid_lkm_userns_adopted_mnt = current->nsproxy ?
						       current->nsproxy->mnt_ns : NULL;
			droid_lkm_misc_info("adopt: unshare-newns %s pid=%d built ns base=%u flags=0x%lx\n",
					    current->comm, current->pid,
					    droid_lkm_userns_base, flags);
		}

		return ret;
	}

	if (!(flags & CLONE_NEWUSER)) {
		/*
		 * a launcher that builds a container asks for the mount namespace, not the
		 * user namespace. record it once: it tells which call actually builds the
		 * container, and whether adopt should hang off this path instead of clone
		 */
		if ((flags & CLONE_NEWNS) && droid_lkm_userns_log_this())
			droid_lkm_misc_info("unshare-newns: comm=%s pid=%d euid=%u flags=0x%lx adopt=%d\n",
					    current->comm, current->pid,
					    __kuid_val(current_euid()), flags,
					    droid_lkm_userns_adopt ? 1 : 0);
		return droid_lkm_userns_unshare_orig(flags);
	}

	if (droid_lkm_userns_log_this())
		droid_lkm_misc_info("unshare: comm=%s pid=%d euid=%u flags=0x%lx any=%d in_ns=%d\n",
				    current->comm, current->pid,
				    __kuid_val(current_euid()), flags,
				    droid_lkm_userns_any,
				    droid_lkm_userns_of(current) ? 1 : 0);

	/* upstream rejects this pair before it touches anything */
	if (flags & CLONE_FS)
		return -EINVAL;

	/*
	 * the flag is dropped for every caller, root included.
	 *
	 * an earlier version let root build a namespace here, on the theory that the
	 * container comes through this path. it does not: the launcher only ever asks
	 * for the mount namespace, and the user namespace it does ask for is a probe.
	 * answering that probe with a real namespace meant rewriting the credential's
	 * user_ns to a namespace this module allocated, and the kernel's
	 * get_user_ns()/put_user_ns() pair does not balance across a change like that
	 * -- the reference count reached zero and the namespace was freed through a
	 * slab cache it never came from.
	 *
	 * so no namespace object is built here and no credential field is touched.
	 * the other namespace bits still go to the kernel, only CLONE_NEWUSER is
	 * removed from the request
	 */
	if (!droid_lkm_userns_any) {
		if (droid_lkm_userns_log_this())
			droid_lkm_misc_info("unshare: %s refused, userns_any is off\n",
					    current->comm);
		return -EINVAL;
	}

	rest = flags & ~CLONE_NEWUSER;
	if (droid_lkm_userns_log_this())
		droid_lkm_misc_info("unshare: %s euid=%u flags=0x%lx -> 0x%lx (flag dropped)\n",
				    current->comm, __kuid_val(current_euid()),
				    flags, rest);

	return droid_lkm_userns_unshare_orig(rest);
}

/*
 * whether this fork is the one that builds a container. the launcher runs as
 * root, is not inside a namespace yet, and is about to create the mount
 * namespace the container lives in. nothing else on the phone matches all
 * three, which is what keeps this from touching an unrelated root process
 */
static bool droid_lkm_userns_adopt_here(unsigned long clone_flags)
{
	return droid_lkm_userns_adopt && droid_lkm_userns_real &&
	       (clone_flags & CLONE_NEWNS) &&
	       !droid_lkm_userns_of(current) &&
	       uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
	       !strncmp(current->comm, "droidspaces", 11);
}

__nocfi noinline int droid_lkm_userns_creds_wrap(struct task_struct *p,
						unsigned long clone_flags)
{
	struct droid_lkm_userns *u;
	unsigned long rest;
	int ret;

	/*
	 * born-in-namespace, step two. the launcher keeps its host rights but shares
	 * the mount namespace that now belongs to the container's user namespace, so
	 * every process it forks from here on is born into that container. the child's
	 * credential is private until it runs, so it is replaced outright
	 */
	if (droid_lkm_userns_adopted_mnt && !(clone_flags & CLONE_THREAD) &&
	    current->nsproxy &&
	    (void *)current->nsproxy->mnt_ns == droid_lkm_userns_adopted_mnt) {
		ret = droid_lkm_userns_creds_orig(p, clone_flags);
		if (ret)
			return ret;

		droid_lkm_userns_install_cred((struct cred *)p->cred,
					      droid_lkm_userns_adopted);
		droid_lkm_misc_info("adopt: fork %s pid=%d born in ns flags=0x%lx\n",
				    current->comm, p->pid, clone_flags);
		return 0;
	}

	/* a thread clone shares credentials, so it cannot own a namespace */
	if ((clone_flags & (CLONE_NEWUSER | CLONE_NEWNS)) == 0 ||
	    (clone_flags & CLONE_THREAD))
		return droid_lkm_userns_creds_orig(p, clone_flags);

	/*
	 * born-in-namespace. copy_creds() runs at the top of copy_process() and
	 * copy_namespaces() runs near the end, and the latter takes the namespace
	 * owner from the child's credential. so a credential changed here is the one
	 * that owns the mount namespace this fork is about to create: the mounts the
	 * container does next are then legal in its own namespace instead of borrowed
	 * from the host, and no capability is handed out that the host would honour
	 */
	if (!(clone_flags & CLONE_NEWUSER) && droid_lkm_userns_adopt_here(clone_flags)) {
		u = droid_lkm_userns_create(NULL);
		if (!u)
			return -ENOMEM;

		ret = droid_lkm_userns_creds_orig(p, clone_flags);
		if (ret) {
			droid_lkm_userns_destroy(u);
			return ret;
		}

		/* the child has not run, its credential is private to it */
		droid_lkm_userns_install_cred((struct cred *)p->cred, u);
		droid_lkm_misc_info("adopt: %s pid=%d born in ns base=%u flags=0x%lx\n",
				    current->comm, current->pid,
				    droid_lkm_userns_base, clone_flags);
		return 0;
	}

	if (droid_lkm_userns_log_this())
		droid_lkm_misc_info("clone: comm=%s pid=%d euid=%u flags=0x%lx any=%d in_ns=%d\n",
				    current->comm, current->pid,
				    __kuid_val(current_euid()), clone_flags,
				    droid_lkm_userns_any,
				    droid_lkm_userns_of(current) ? 1 : 0);

	/* same answer as unshare: the flag is dropped for root as well */
	rest = clone_flags & ~CLONE_NEWUSER;

	if (!droid_lkm_userns_any) {
		if (droid_lkm_userns_log_this())
			droid_lkm_misc_info("clone: %s refused, userns_any is off\n",
					    current->comm);
		return -EINVAL;
	}

	if (droid_lkm_userns_log_this())
		droid_lkm_misc_info("clone: %s euid=%u flags=0x%lx -> 0x%lx (flag dropped)\n",
				    current->comm, __kuid_val(current_euid()),
				    clone_flags, rest);

	return droid_lkm_userns_creds_orig(p, rest);
}

__nocfi noinline bool droid_lkm_userns_inodecap_wrap(struct mnt_idmap *idmap,
						     const struct inode *inode,
						     int cap)
{
	struct droid_lkm_userns *u;

	if (droid_lkm_userns_inodecap_orig(idmap, inode, cap))
		return true;

	/* an idmapped mount is a different rule set, it arrives with the lens */
	if (droid_lkm_misc_ks.nop_mnt_idmap && idmap != droid_lkm_misc_ks.nop_mnt_idmap)
		return false;

	u = droid_lkm_userns_of(current);
	if (!u)
		return false;

	if (!cap_raised(current_cred()->cap_effective, cap))
		return false;

	/* privileged_wrt_inode_uidgid(): both ids must be mapped in the namespace */
	/*
	 * the inode carries a raw kuid, so the reverse direction answers this: an inode
	 * owned by an id outside the container's range is not privileged, no matter how
	 * full cap_effective is. with the identity map this is the same test as before,
	 * with a real map it is what stops a container root from taking over a host file
	 */
	return droid_lkm_userns_map_up(u, __kuid_val(inode->i_uid)) != (u32)-1 &&
	       droid_lkm_userns_map_up(u, __kgid_val(inode->i_gid)) != (u32)-1;
}

static int droid_lkm_userns_hook_one(struct hk_inline *h, const char *sym,
				     const char *wrap, void **orig)
{
	int ret;

	ret = hk_inline_hook(h, sym, wrap);
	if (ret) {
		droid_lkm_misc_warn("hook %s failed: %d\n", sym, ret);
		return ret;
	}

	*orig = (void *)h->orig;
	droid_lkm_misc_dbg("hook %-28s addr=0x%lx orig=0x%lx\n", sym, h->addr, h->orig);

	return 0;
}

int droid_lkm_userns_hooks_init(void)
{
	int ret;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_unshare_hook, "ksys_unshare",
		"droid_lkm_userns_unshare_wrap", (void **)&droid_lkm_userns_unshare_orig);
	if (ret)
		return ret;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_nsproxy_hook,
		"unshare_nsproxy_namespaces", "droid_lkm_userns_nsproxy_wrap",
		(void **)&droid_lkm_userns_nsproxy_orig);
	if (ret)
		goto out_unshare;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_creds_hook, "copy_creds",
		"droid_lkm_userns_creds_wrap", (void **)&droid_lkm_userns_creds_orig);
	if (ret)
		goto out_nsproxy;

	/*
	 * both of these only matter when userns_adopt is on, so a kernel that does not
	 * export them is not a reason to fail the whole module: the container keeps
	 * working the way it did before, just without its own id space
	 */
	if (droid_lkm_userns_hook_one(&droid_lkm_userns_kclone_hook, "kernel_clone",
		"droid_lkm_userns_kclone_wrap", (void **)&droid_lkm_userns_kclone_orig))
		droid_lkm_misc_warn("kernel_clone not hookable, adopt falls back to exec\n");

	if (droid_lkm_userns_hook_one(&droid_lkm_userns_exec_hook, "do_execve",
		"droid_lkm_userns_exec_wrap", (void **)&droid_lkm_userns_exec_orig))
		droid_lkm_misc_warn("do_execve not hookable, adopt may not reach the container\n");

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_inodecap_hook,
		"capable_wrt_inode_uidgid", "droid_lkm_userns_inodecap_wrap",
		(void **)&droid_lkm_userns_inodecap_orig);
	if (ret)
		goto out_creds;

	droid_lkm_misc_info("userns hooks installed\n");
	return 0;

out_creds:
	hk_inline_unhook(&droid_lkm_userns_creds_hook);
out_nsproxy:
	hk_inline_unhook(&droid_lkm_userns_nsproxy_hook);
out_unshare:
	hk_inline_unhook(&droid_lkm_userns_unshare_hook);
	return ret;
}

void droid_lkm_userns_hooks_exit(void)
{
	hk_inline_unhook(&droid_lkm_userns_exec_hook);
	hk_inline_unhook(&droid_lkm_userns_kclone_hook);
	hk_inline_unhook(&droid_lkm_userns_inodecap_hook);
	hk_inline_unhook(&droid_lkm_userns_creds_hook);
	hk_inline_unhook(&droid_lkm_userns_nsproxy_hook);
	hk_inline_unhook(&droid_lkm_userns_unshare_hook);
}
