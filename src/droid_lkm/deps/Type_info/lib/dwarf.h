// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_DWARF_H
#define TYPEMOD_DWARF_H

#include <linux/types.h>

struct ti_dw_member {
	u32 bit_off;
	u32 bit_sz;
	char *name;
};

struct ti_dw_struct {
	char *name;
	struct ti_dw_member *m;
	u32 n;
	u32 cap;
};

struct ti_dw {
	void *info;
	u32 info_sz;
	void *abbrev;
	u32 abbrev_sz;
	void *str;
	u32 str_sz;
	void *stroff;
	u32 stroff_sz;
	struct ti_dw_struct *st;
	u32 cnt;
	u32 cap;
};

int ti_dw_capture(struct ti_dw *dw, const void *btf_data);
void ti_dw_free(struct ti_dw *dw);
int ti_dw_member_off(const struct ti_dw *dw, const char *sname,
		     const char *member, u32 *bit_off, u32 *bit_sz);
const char *ti_dw_member_name(const struct ti_dw *dw, const char *sname,
			      u32 bit_off, u32 bit_sz);

#endif
