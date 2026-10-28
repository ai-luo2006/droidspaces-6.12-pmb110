// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "reg.h"
#include "type_info.h"

static struct ti_reg_ent ti_reg[TI_REG_MAX_STRUCT];
static struct mutex ti_reg_lock;

int ti_reg_init(void)
{
	memset(ti_reg, 0, sizeof(ti_reg));
	mutex_init(&ti_reg_lock);
	return 0;
}

void ti_reg_exit(void)
{
	memset(ti_reg, 0, sizeof(ti_reg));
}

int ti_reg_add(const char *name, u32 size, const struct ti_member_desc *m,
	       u32 n)
{
	int idx = -1;
	u32 i;

	if (!name || !m || n > TI_REG_MAX_MEM)
		return -EINVAL;

	mutex_lock(&ti_reg_lock);
	for (i = 0; i < TI_REG_MAX_STRUCT; i++) {
		if (!ti_reg[i].name[0]) {
			idx = i;
			break;
		}
		if (!strcmp(ti_reg[i].name, name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		goto out;
	strscpy(ti_reg[idx].name, name, sizeof(ti_reg[idx].name));
	ti_reg[idx].size = size;
	ti_reg[idx].n = n;
	for (i = 0; i < n; i++) {
		strscpy(ti_reg[idx].mem[i].name, m[i].name,
			sizeof(ti_reg[idx].mem[i].name));
		ti_reg[idx].mem[i].bit_off = m[i].bit_off;
		ti_reg[idx].mem[i].bit_sz = m[i].bit_sz;
	}
out:
	mutex_unlock(&ti_reg_lock);
	return idx;
}

void ti_reg_del(const char *name)
{
	int idx;
	u32 i;

	mutex_lock(&ti_reg_lock);
	idx = -1;
	for (i = 0; i < TI_REG_MAX_STRUCT; i++) {
		if (ti_reg[i].name[0] && !strcmp(ti_reg[i].name, name)) {
			idx = i;
			break;
		}
	}
	if (idx >= 0)
		memset(&ti_reg[idx], 0, sizeof(ti_reg[idx]));
	mutex_unlock(&ti_reg_lock);
}

int ti_reg_lookup(const char *name)
{
	u32 i;
	int ret = -ENOENT;

	if (!name)
		return -EINVAL;
	mutex_lock(&ti_reg_lock);
	for (i = 0; i < TI_REG_MAX_STRUCT; i++) {
		if (ti_reg[i].name[0] && !strcmp(ti_reg[i].name, name)) {
			ret = i;
			break;
		}
	}
	mutex_unlock(&ti_reg_lock);
	return ret;
}

int ti_reg_id_to_idx(u32 id)
{
	u32 idx = id - TI_REG_ID_BASE;

	if (id < TI_REG_ID_BASE || idx >= TI_REG_MAX_STRUCT)
		return -ENOENT;
	if (!ti_reg[idx].name[0])
		return -ENOENT;
	return idx;
}

u32 ti_reg_idx_to_id(u32 idx)
{
	return TI_REG_ID_BASE + idx;
}

u32 ti_reg_size(u32 idx)
{
	return ti_reg[idx].size;
}

u32 ti_reg_mem_count(u32 idx)
{
	return ti_reg[idx].n;
}

int ti_reg_mem_at(u32 idx, u32 i, const char **name, u32 *bit_off,
		  u32 *bit_sz)
{
	if (i >= ti_reg[idx].n)
		return -ENOENT;
	if (name)
		*name = ti_reg[idx].mem[i].name;
	*bit_off = ti_reg[idx].mem[i].bit_off;
	*bit_sz = ti_reg[idx].mem[i].bit_sz;
	return 0;
}

int ti_reg_member(u32 idx, const char *member, u32 *bit_off, u32 *bit_sz)
{
	u32 i;

	if (!member || !bit_off || !bit_sz)
		return -EINVAL;
	for (i = 0; i < ti_reg[idx].n; i++) {
		if (strcmp(ti_reg[idx].mem[i].name, member))
			continue;
		*bit_off = ti_reg[idx].mem[i].bit_off;
		*bit_sz = ti_reg[idx].mem[i].bit_sz;
		return 0;
	}
	return -ENOENT;
}

int ti_reg_struct(const char *name, u32 size,
		  const struct ti_member_desc *members, u32 n)
{
	int ret = ti_reg_add(name, size, members, n);

	return ret < 0 ? ret : 0;
}
#ifdef CONFIG_TI_REG
EXPORT_SYMBOL(ti_reg_struct);
#endif

void ti_unreg_struct(const char *name)
{
	ti_reg_del(name);
}
#ifdef CONFIG_TI_REG
EXPORT_SYMBOL(ti_unreg_struct);
#endif
