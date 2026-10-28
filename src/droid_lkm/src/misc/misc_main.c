// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/init.h>

#include "core.h"
#include "hk.h"
#include "misc.h"
#include "misc_ksym.h"
#include "misc_proc.h"
#include "xt_reg.h"
#include "devtmpfs/devtmpfs.h"
#include "userns/userns.h"

bool droid_lkm_misc_verbose;

static unsigned long __nocfi droid_lkm_misc_hk_resolve(const char *name)
{
	return kallrecon_klp ? kallrecon_klp(name) : 0;
}

static const struct hk_cfg droid_lkm_misc_hk_cfg = {
	.resolve = droid_lkm_misc_hk_resolve,
};

static bool droid_lkm_misc_xt_enable = true;
static bool droid_lkm_misc_userns_enable = true;
static bool droid_lkm_misc_devtmpfs_enable = true;

module_param_named(verbose, droid_lkm_misc_verbose, bool, 0444);
MODULE_PARM_DESC(verbose, "log symbol probes and every registered item");

module_param_named(xt, droid_lkm_misc_xt_enable, bool, 0444);
MODULE_PARM_DESC(xt, "register the ported xt matches and targets");

module_param_named(userns, droid_lkm_misc_userns_enable, bool, 0444);
MODULE_PARM_DESC(userns, "offer the /proc/<pid>/ns/user entry and the fake user namespace");

module_param_named(devtmpfs, droid_lkm_misc_devtmpfs_enable, bool, 0444);
MODULE_PARM_DESC(devtmpfs, "offer the devtmpfs filesystem the kernel was built without");

module_param_named(captrace, droid_lkm_userns_captrace_on, bool, 0444);
MODULE_PARM_DESC(captrace, "log every capability decision refused for a container task");

module_param_named(real_userns, droid_lkm_userns_real, bool, 0444);
MODULE_PARM_DESC(real_userns,
		 "give a container its own uid space: id 0 inside is a shifted host uid");

module_param_named(userns_any, droid_lkm_userns_any, bool, 0444);
MODULE_PARM_DESC(userns_any,
		 "let any task create a namespace, not only tasks that already have one or start from root");

module_param_named(userns_adopt, droid_lkm_userns_adopt, bool, 0444);
MODULE_PARM_DESC(userns_adopt,
		 "put a container in a real user namespace at the fork that builds it, without the launcher asking");

module_param_named(userns_base, droid_lkm_userns_base, uint, 0444);
MODULE_PARM_DESC(userns_base,
		 "host uid that container id 0 maps to; the rootfs must be chowned to it once");

module_param_named(userns_spread, droid_lkm_userns_spread, bool, 0444);
MODULE_PARM_DESC(userns_spread,
		 "give each new namespace its own 70000 wide range instead of sharing userns_base");

static bool droid_lkm_misc_userns_up;
static bool droid_lkm_misc_hk_up;

/*
 * write 1 to drop devtmpfs' internal mount, which is what keeps a module reference alive
 * and rmmod refused
 */
/*
 * release=1 drops the devtmpfs internal mount, which is what lets the module
 * be unloaded. release=0 puts it back: dropping the mount unregisters the
 * filesystem, and a kernel without CONFIG_DEVTMPFS has no other way to get
 * one, so without a way back the file system stays gone for as long as the
 * module is loaded.
 */
static int droid_lkm_misc_release_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret;

	if (kstrtobool(val, &on))
		return -EINVAL;

	if (on) {
		unsigned int busy = droid_lkm_userns_busy();

		/*
		 * 有容器还在用这个命名空间时，泄掉对象也没用：cred->user_ns 指着
		 * 模块内存，rmmod 之后任何一次能力检查都会踩空。这里直接拒绝，
		 * 让 rmmod 因为引用计数非零而失败，比崩一次强
		 */
		if (busy) {
			droid_lkm_misc_warn("release refused: %u namespace(s) still held by tasks, stop the container first\n",
					    busy);
			return -EBUSY;
		}

		/*
		 * 撤探针必须发生在 rmmod 之前：getuid 是全系统最热的 syscall，
		 * 模块代码一旦被 unmap，还挂着的探针立刻就是野指针
		 */
		droid_lkm_userns_view_exit();
		return droid_lkm_misc_devtmpfs_release();
	}

	ret = droid_lkm_misc_devtmpfs_init();
	if (ret) {
		droid_lkm_misc_warn("devtmpfs restore failed: %d\n", ret);
		return ret;
	}

	/* release=0 把功能恢复回来，探针一起装回去 */
	droid_lkm_userns_view_init();

	droid_lkm_misc_report("devtmpfs", "restored", "internal mount back");
	return 0;
}

static const struct kernel_param_ops droid_lkm_misc_release_ops = {
	.set	= droid_lkm_misc_release_set,
};

module_param_cb(release, &droid_lkm_misc_release_ops, NULL, 0200);
MODULE_PARM_DESC(release, "write 1 to drop the devtmpfs internal mount before unloading, write 0 to put it back");

static int __init droid_lkm_misc_init(void)
{
	int ret;

	find_kallsyms_base();
	if (!klnum_val || !kallrecon_klp) {
		droid_lkm_misc_err("kallsyms recovery failed\n");
		return -ENODATA;
	}

	droid_lkm_misc_info("loaded\n");
	droid_lkm_misc_ksym_probe();

	ret = droid_lkm_misc_ksym_init();
	if (ret) {
		droid_lkm_misc_err("thunk table incomplete: %d\n", ret);
		return ret;
	}

	ret = hk_init(&droid_lkm_misc_hk_cfg);
	if (ret) {
		droid_lkm_misc_err("hk_init failed: %d\n", ret);
		return ret;
	}
	droid_lkm_misc_hk_up = true;

	ret = droid_lkm_misc_proc_init();
	if (ret)
		droid_lkm_misc_warn("control plane unavailable: %d\n", ret);

	if (droid_lkm_misc_xt_enable) {
		ret = droid_lkm_misc_xt_init();
		if (ret)
			return ret;
	} else {
		droid_lkm_misc_report("xt", "off", "xt=0");
	}

	if (droid_lkm_misc_devtmpfs_enable) {
		ret = droid_lkm_misc_devtmpfs_init();
		if (ret)
			droid_lkm_misc_warn("devtmpfs unavailable: %d\n", ret);
	} else {
		droid_lkm_misc_report("devtmpfs", "off", "devtmpfs=0");
	}

	if (droid_lkm_misc_userns_enable) {
		ret = droid_lkm_userns_init();
		if (ret)
			droid_lkm_misc_warn("userns unavailable: %d\n", ret);
		else
			droid_lkm_misc_userns_up = true;
	} else {
		droid_lkm_misc_report("userns", "off", "userns=0");
	}

	return 0;
}

static void __exit droid_lkm_misc_exit(void)
{
	if (droid_lkm_misc_userns_up) {
		droid_lkm_userns_exit();
		droid_lkm_misc_userns_up = false;
	}
	droid_lkm_misc_devtmpfs_exit();
	droid_lkm_misc_xt_exit();
	droid_lkm_misc_proc_exit();
	if (droid_lkm_misc_hk_up) {
		hk_exit();
		droid_lkm_misc_hk_up = false;
	}
	droid_lkm_misc_info("unloaded\n");
}

module_init(droid_lkm_misc_init);
module_exit(droid_lkm_misc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("container scoped kernel features missing on the stock GKI build");
