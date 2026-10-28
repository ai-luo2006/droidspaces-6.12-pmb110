// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * The install time self test, three layers.
 *
 * Layer one sweeps the map functions over their whole input domain and lives
 * next to them, in userns_map.c. Layer two checks that nothing the module
 * promised reported itself refused. Layer three walks the path a container
 * takes, from a kernel thread: take a user namespace, take a uts namespace and
 * check that it is owned by it, take a mount namespace, and make the mount that
 * a namespace owned by the host cannot make.
 *
 * Layer three is the one that matters, because it runs the same code a
 * container runs, on the device, before any container exists. A device where
 * this fails is a device where the first container to mount something would
 * have failed, and it says so in the log instead. It runs in its own thread
 * with its own namespaces and disappears with them, so nothing outside is
 * touched.
 */

#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/utsname.h>
#include <linux/nsproxy.h>
#include <linux/mnt_namespace.h>

#include "misc.h"
#include "misc_ksym.h"
#include "misc_proc.h"
#include "userns.h"

static int (*droid_lkm_userns_selftest_unshare)(unsigned long flags);
static int (*droid_lkm_userns_selftest_kern_path)(const char *name,
		unsigned int flags, struct path *path);
static int (*droid_lkm_userns_selftest_path_mount)(const char *dev_name,
		struct path *path, const char *type, unsigned long flags,
		void *data);
static void (*droid_lkm_userns_selftest_path_put)(const struct path *path);

static unsigned int droid_lkm_userns_selftest_failed;
static struct completion droid_lkm_userns_selftest_done;

/* the last failure, named, so the status file answers "which step" by itself */
static char droid_lkm_userns_selftest_where[40] = "none";
static int droid_lkm_userns_selftest_errno;
static char droid_lkm_userns_selftest_skipped[40];

static void droid_lkm_userns_selftest_step(const char *what, int ok, int ret)
{
	if (ok)
		return;

	droid_lkm_userns_selftest_failed++;
	strscpy(droid_lkm_userns_selftest_where, what,
		sizeof(droid_lkm_userns_selftest_where));
	droid_lkm_userns_selftest_errno = ret;
	droid_lkm_misc_err("selftest: %s failed: %d\n", what, ret);
}

/*
 * a mount made from a kernel thread runs in the kernel's own security context, so
 * an LSM may refuse it for a reason that has nothing to do with the namespace. it
 * is reported, and it is not counted: a container mounts from its own domain
 */
static void droid_lkm_userns_selftest_skip(const char *what, int ret)
{
	strscpy(droid_lkm_userns_selftest_skipped, what,
		sizeof(droid_lkm_userns_selftest_skipped));
	droid_lkm_misc_warn("selftest: %s skipped, LSM refused it in the kernel domain: %d\n",
			    what, ret);
}

/*
 * the sample: a user namespace of this module's making, a uts namespace recording it as
 * owner, a mount namespace, and a tmpfs mounted in it
 */
static int droid_lkm_userns_selftest_thread(void *unused)
{
	struct droid_lkm_userns *u;
	struct path path;
	char *data;
	int ret;

	if (droid_lkm_userns_of(current))
		droid_lkm_userns_selftest_step("starts outside a namespace", 0, 0);

	ret = droid_lkm_userns_selftest_unshare(CLONE_NEWUSER);
	u = droid_lkm_userns_of(current);
	droid_lkm_userns_selftest_step("unshare(CLONE_NEWUSER)", ret == 0 && u, ret);

	if (!u)
		goto out;

	ret = droid_lkm_userns_selftest_unshare(CLONE_NEWUTS);
	droid_lkm_userns_selftest_step("unshare(CLONE_NEWUTS)", ret == 0, ret);
	if (!ret)
		droid_lkm_userns_selftest_step("the uts namespace is owned by it",
			current->nsproxy->uts_ns->user_ns == &u->uns, 0);

	ret = droid_lkm_userns_selftest_unshare(CLONE_NEWNS);
	droid_lkm_userns_selftest_step("unshare(CLONE_NEWNS)", ret == 0, ret);
	if (ret)
		goto out;

	/* path_mount() writes the last byte of the data page, so this needs a page of its own */
	data = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!data)
		goto out;
	strscpy(data, "size=1k", PAGE_SIZE);

	ret = droid_lkm_userns_selftest_kern_path("/", LOOKUP_FOLLOW, &path);
	if (ret) {
		droid_lkm_userns_selftest_step("resolve /", 0, ret);
		kfree(data);
		goto out;
	}

	ret = droid_lkm_userns_selftest_path_mount("none", &path, "tmpfs", 0, data);
	if (ret == -EACCES)
		droid_lkm_userns_selftest_skip("mount tmpfs in the namespace it owns", ret);
	else
		droid_lkm_userns_selftest_step("mount tmpfs in the namespace it owns",
					       ret == 0, ret);

	droid_lkm_userns_selftest_path_put(&path);
	kfree(data);

out:
	complete(&droid_lkm_userns_selftest_done);

	return 0;
}

static int droid_lkm_userns_selftest_e2e(void)
{
	struct task_struct *task;

	task = kthread_run(droid_lkm_userns_selftest_thread, NULL, "dlm_userns_probe");
	if (IS_ERR(task)) {
		droid_lkm_misc_err("selftest: no thread: %ld\n", PTR_ERR(task));
		return PTR_ERR(task);
	}

	if (!wait_for_completion_timeout(&droid_lkm_userns_selftest_done, 10 * HZ)) {
		droid_lkm_misc_err("selftest: timed out\n");
		droid_lkm_userns_selftest_failed++;
	}

	return 0;
}

static int droid_lkm_userns_selftest_resolve(void)
{
	struct {
		const char *name;
		void **slot;
	} syms[] = {
		{ "ksys_unshare",	(void **)&droid_lkm_userns_selftest_unshare },
		{ "kern_path",		(void **)&droid_lkm_userns_selftest_kern_path },
		{ "path_mount",		(void **)&droid_lkm_userns_selftest_path_mount },
		{ "path_put",		(void **)&droid_lkm_userns_selftest_path_put },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(syms); i++) {
		*syms[i].slot = (void *)droid_lkm_misc_sym(syms[i].name);
		if (!*syms[i].slot) {
			droid_lkm_misc_warn("selftest: %s not found\n", syms[i].name);
			return -ENOENT;
		}
	}

	return 0;
}

/* layer two counts the refusals the report already holds */
static void droid_lkm_userns_selftest_coverage(void)
{
	unsigned int refused = droid_lkm_misc_report_count("unsupported");

	droid_lkm_userns_selftest_failed += refused;

	if (refused)
		droid_lkm_misc_err("selftest: %u features reported unsupported\n",
				   refused);
}

void droid_lkm_userns_selftest_run(void)
{
	char detail[64];
	int ret;

	droid_lkm_userns_selftest_coverage();

	ret = droid_lkm_userns_selftest_resolve();
	if (ret) {
		droid_lkm_misc_report("userns selftest", "unsupported",
				      "a symbol for the sample is missing");
		return;
	}

	init_completion(&droid_lkm_userns_selftest_done);
	droid_lkm_userns_selftest_e2e();

	if (droid_lkm_userns_selftest_failed) {
		scnprintf(detail, sizeof(detail), "%u failed: %s (%d)",
			  droid_lkm_userns_selftest_failed,
			  droid_lkm_userns_selftest_where,
			  droid_lkm_userns_selftest_errno);
		droid_lkm_misc_report("userns selftest", "failed", detail);
		droid_lkm_misc_err("userns selftest failed: %s\n", detail);
		return;
	}

	if (droid_lkm_userns_selftest_skipped[0]) {
		scnprintf(detail, sizeof(detail), "3 layers, mount skipped by LSM");
		droid_lkm_misc_report("userns selftest", "ready", detail);
		droid_lkm_misc_info("userns selftest passed, mount skipped in kernel domain\n");
		return;
	}

	droid_lkm_misc_report("userns selftest", "ready",
			      "3 layers, no failures");
	droid_lkm_misc_info("userns selftest passed\n");
}
