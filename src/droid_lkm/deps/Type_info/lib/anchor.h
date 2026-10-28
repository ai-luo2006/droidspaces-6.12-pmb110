// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_ANCHOR_H
#define TYPEMOD_ANCHOR_H

#include <linux/types.h>

struct ti_task_offs {
	u32 off_pid;
	u32 off_comm;
	u32 off_real_cred;
	u32 off_cred;
	u32 off_mm;
	u32 off_tasks;
	u32 mm_pgd;
	u32 mm_arg_start;
};

struct ti_boot_args {
	pid_t pid;
	pid_t tgid;
	const char comm[16];
	pid_t ref_pid;
	pid_t ref_tgid;
};

struct ti_module_offs {
	u32 off_list;
	u32 off_name;
	u32 off_state;
	u32 off_core_base;
	u32 off_core_size;
	u32 off_btf_data;
	u32 off_btf_data_size;
	u32 off_btf_base_data;
	u32 off_btf_base_data_size;
};

void ti_anchor_init(unsigned long (*resolve)(const char *name));

int ti_scan_bytes(const void *base, u32 range, const void *sample, u32 len,
		  u32 *off);
int ti_scan_u32(const void *base, u32 range, u32 val, u32 *off);
int ti_scan_ptrpair(const void *base, u32 range, u32 *off);
int ti_scan_ptrpair_rev(const void *base, u32 range, u32 *off);

int ti_bootstrap_task(const struct ti_boot_args *args, struct ti_task_offs *out);
int ti_bootstrap_module(struct ti_module_offs *out);

#endif
