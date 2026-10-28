// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_TYPE_INFO_H
#define TYPEMOD_TYPE_INFO_H

#include <linux/types.h>

#ifdef CONFIG_TI_DWARF_EXPORT
#include "dwarf.h"
#endif

struct ti_resolver {
	unsigned long (*name_to_addr)(const char *name);
};

struct ti_ctx;

enum ti_btf_mode {
	TI_BTF_AUTO = 0,
	TI_BTF_DISTILLED = 1,
	TI_BTF_SPLIT = 2,
	TI_BTF_STANDALONE = 3,
};

struct ti_module {
	const char *name;
	unsigned int state;
	unsigned long core_base;
	unsigned long core_size;
	void *mod;
};

int ti_init(struct ti_resolver *res);
void ti_exit(void);
struct ti_ctx *ti_base(void);
bool ti_ready(void);
bool ti_btf_available(void);
u32 ti_type_count(void);

int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out);
void ti_ctx_close(struct ti_ctx *ctx);

int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
		    u32 kind_mask, u32 *out);
u32 ti_type_size(const struct ti_ctx *ctx, u32 id);
int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out);
int ti_member_off(const struct ti_ctx *ctx, u32 id, const char *member,
		  u32 *bit_off, u32 *bit_sz);
int ti_member_info(const struct ti_ctx *ctx, u32 id, const char *member,
		   u32 *type, u32 *bit_off, u32 *bit_sz);
int ti_member_count(const struct ti_ctx *ctx, u32 id);
int ti_member_at(const struct ti_ctx *ctx, u32 id, u32 idx,
		 const char **name, u32 *type, u32 *bit_off, u32 *bit_sz);

#ifdef CONFIG_TI_FUNC
struct ti_type_raw {
	u32 kind;
	u32 vlen;
	u32 type;
	const char *name;
	const void *data;
	u32 data_len;
};

int ti_func_proto(const struct ti_ctx *ctx, u32 func_id,
		  u32 *ret_type, u32 *nparams);
int ti_func_param(const struct ti_ctx *ctx, u32 func_id, u32 idx,
		  const char **name, u32 *type);
int ti_enum_at(const struct ti_ctx *ctx, u32 id, u32 idx,
	       const char **name, s64 *val);
int ti_enum_val(const struct ti_ctx *ctx, u32 id, const char *name,
		s64 *val);
int ti_type_raw(const struct ti_ctx *ctx, u32 id, struct ti_type_raw *out);
#endif

int ti_mod_lookup(const char *name, struct ti_ctx **out);
int ti_mod_enum(int (*cb)(const struct ti_module *m, void *arg), void *arg);

struct ti_member_desc {
	const char *name;
	u32 bit_off;
	u32 bit_sz;
};

int ti_reg_struct(const char *name, u32 size,
		  const struct ti_member_desc *members, u32 n);
void ti_unreg_struct(const char *name);

int ti_anchor_set_modname(const char *name);

#ifdef CONFIG_TI_FEATURE
int ti_type_by_size(const struct ti_ctx *ctx, u32 size, u32 vlen,
		    u32 *out);
int ti_type_by_seq(const struct ti_ctx *ctx, u32 size,
		   const struct ti_member_desc *seq, u32 n, u32 *out);
#endif

#ifdef CONFIG_TI_PUBLIC_ANCHOR
int ti_safe_read(void *dst, const void *src, size_t sz);
int ti_scan_bytes(const void *base, u32 range, const void *sample, u32 len,
		  u32 *off);
int ti_scan_u32(const void *base, u32 range, u32 val, u32 *off);
int ti_scan_ptrpair(const void *base, u32 range, u32 *off);
int ti_scan_ptrpair_rev(const void *base, u32 range, u32 *off);
#endif

#endif
