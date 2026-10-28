// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_BTF_H
#define TYPEMOD_BTF_H

#include <linux/types.h>

#define BTF_MAGIC	0xeB9F

#define BTF_INFO_KIND(info)	(((info) >> 24) & 0x1f)
#define BTF_INFO_VLEN(info)	((info) & 0xffff)
#define BTF_INFO_KFLAG(info)	((info) >> 31)

#define BTF_MEMBER_BIT_OFF(off)	((off) & 0xffffff)
#define BTF_MEMBER_BIT_SZ(off)	((off) >> 24)

enum {
	BTF_KIND_UNKN = 0,
	BTF_KIND_INT = 1,
	BTF_KIND_PTR = 2,
	BTF_KIND_ARRAY = 3,
	BTF_KIND_STRUCT = 4,
	BTF_KIND_UNION = 5,
	BTF_KIND_ENUM = 6,
	BTF_KIND_FWD = 7,
	BTF_KIND_TYPEDEF = 8,
	BTF_KIND_VOLATILE = 9,
	BTF_KIND_CONST = 10,
	BTF_KIND_RESTRICT = 11,
	BTF_KIND_FUNC = 12,
	BTF_KIND_FUNC_PROTO = 13,
	BTF_KIND_VAR = 14,
	BTF_KIND_DATASEC = 15,
	BTF_KIND_FLOAT = 16,
	BTF_KIND_DECL_TAG = 17,
	BTF_KIND_TYPE_TAG = 18,
	BTF_KIND_ENUM64 = 19,
};

#define BTF_KIND_QUAL	(BIT(BTF_KIND_CONST) | BIT(BTF_KIND_VOLATILE) | \
			 BIT(BTF_KIND_RESTRICT) | BIT(BTF_KIND_TYPE_TAG))

struct btf_header {
	__u16	magic;
	__u8	version;
	__u8	flags;
	__u32	hdr_len;
	__u32	type_off;
	__u32	type_len;
	__u32	str_off;
	__u32	str_len;
};

struct btf_type {
	__u32	name_off;
	__u32	info;
	union {
		__u32	size;
		__u32	type;
	};
};

struct btf_member {
	__u32	name_off;
	__u32	type;
	__u32	offset;
};

struct btf_array {
	__u32	type;
	__u32	index_type;
	__u32	nelems;
};

struct btf_enum {
	__u32	name_off;
	__s32	val;
};

struct btf_param {
	__u32	name_off;
	__u32	type;
};

struct btf_var {
	__u32	linkage;
};

struct btf_var_secinfo {
	__u32	type;
	__u32	offset;
	__u32	size;
};

struct btf_decl_tag {
	__s32	component_idx;
};

struct btf_enum64 {
	__u32	name_off;
	__u32	val_lo32;
	__u32	val_hi32;
};

struct btf_cursor {
	const void *blob;
	u32 blob_size;
	u32 hdr_len;
	u32 type_off;
	u32 type_len;
	u32 str_off;
	u32 str_len;
	u32 type_cnt;
	u32 id_off;
	u32 str_base_off;
	u32 *type_offs;
	const char *strs;
};

int btf_open(const void *blob, u32 size, struct btf_cursor *c);
void btf_close(struct btf_cursor *c);
const struct btf_type *btf_type(const struct btf_cursor *c, u32 id);
const char *btf_str(const struct btf_cursor *c, u32 off);

#endif
