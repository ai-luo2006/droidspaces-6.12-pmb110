// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>

#include "misc_scope.h"

/*
 * a container task runs under a namespace object this project made, the fake user namespace
 * or the fake pid namespace. both fields exist with CONFIG_USER_NS=n and CONFIG_PID_NS=n, so
 * scope needs no cross module call
 */
bool droid_lkm_misc_is_container_task(struct task_struct *task)
{
	if (!task)
		return false;

	if (__task_cred(task)->user_ns != &init_user_ns)
		return true;

	return task_active_pid_ns(task) != &init_pid_ns;
}

bool droid_lkm_misc_is_container_net(const struct net *net)
{
	if (!net || net == &init_net)
		return false;

	return net->user_ns != &init_user_ns;
}
