// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_REG_H
#define TYPEMOD_REG_H

#include <linux/types.h>

#include "type_info.h"

#define TI_REG_MAX_STRUCT	32
#define TI_REG_MAX_MEM		64
#define TI_REG_ID_BASE		0x80000000u

struct ti_reg_ent {
	char name[64];
	u32 size;
	struct {
		char name[64];
		u32 bit_off;
		u32 bit_sz;
	} mem[TI_REG_MAX_MEM];
	u32 n;
};

int ti_reg_init(void);
void ti_reg_exit(void);
int ti_reg_add(const char *name, u32 size, const struct ti_member_desc *m,
	       u32 n);
void ti_reg_del(const char *name);
int ti_reg_lookup(const char *name);
int ti_reg_id_to_idx(u32 id);
u32 ti_reg_idx_to_id(u32 idx);
u32 ti_reg_size(u32 idx);
u32 ti_reg_mem_count(u32 idx);
int ti_reg_mem_at(u32 idx, u32 i, const char **name, u32 *bit_off,
		  u32 *bit_sz);
int ti_reg_member(u32 idx, const char *member, u32 *bit_off, u32 *bit_sz);

#endif
