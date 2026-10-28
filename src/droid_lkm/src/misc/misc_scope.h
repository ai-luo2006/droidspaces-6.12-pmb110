// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef DROID_LKM_MISC_SCOPE_H
#define DROID_LKM_MISC_SCOPE_H

#include <linux/types.h>

struct task_struct;
struct net;

/* container membership, call with rcu_read_lock() held or on a pinned task */
bool droid_lkm_misc_is_container_task(struct task_struct *task);
bool droid_lkm_misc_is_container_net(const struct net *net);

#endif
