// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/string.h>

#include "btf.h"
#include "port.h"

static u32 btf_extra(u32 kind, u32 vlen)
{
	switch (kind) {
	case BTF_KIND_INT:
		return sizeof(u32);
	case BTF_KIND_ENUM:
		return vlen * sizeof(struct btf_enum);
	case BTF_KIND_ARRAY:
		return sizeof(struct btf_array);
	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION:
		return vlen * sizeof(struct btf_member);
	case BTF_KIND_FUNC_PROTO:
		return vlen * sizeof(struct btf_param);
	case BTF_KIND_VAR:
		return sizeof(struct btf_var);
	case BTF_KIND_DATASEC:
		return vlen * sizeof(struct btf_var_secinfo);
	case BTF_KIND_DECL_TAG:
		return sizeof(struct btf_decl_tag);
	case BTF_KIND_ENUM64:
		return vlen * sizeof(struct btf_enum64);
	default:
		return 0;
	}
}

int btf_open(const void *blob, u32 size, struct btf_cursor *c)
{
	struct btf_header h;
	const char *base;
	const char *p;
	const char *end;
	u32 i;

	memset(c, 0, sizeof(*c));
	if (!blob || size < sizeof(struct btf_header))
		return -EINVAL;

	if (ti_safe_read(&h, blob, sizeof(h)))
		return -EINVAL;
	if (h.magic != BTF_MAGIC || h.version != 1 || h.hdr_len < sizeof(h))
		return -EINVAL;

	base = (const char *)blob + h.hdr_len;
	if (h.type_off + h.type_len > size - h.hdr_len ||
	    h.str_off + h.str_len > size - h.hdr_len)
		return -EINVAL;

	c->blob = blob;
	c->blob_size = size;
	c->hdr_len = h.hdr_len;
	c->type_off = h.type_off;
	c->type_len = h.type_len;
	c->str_off = h.str_off;
	c->str_len = h.str_len;
	c->strs = base + h.str_off;

	p = base + h.type_off;
	end = p + h.type_len;
	while (p < end) {
		const struct btf_type *t = (const struct btf_type *)p;
		u32 extra;

		if (p + sizeof(*t) > end)
			return -EINVAL;
		extra = btf_extra(BTF_INFO_KIND(t->info), BTF_INFO_VLEN(t->info));
		p += sizeof(*t) + extra;
		c->type_cnt++;
		if (c->type_cnt > 0xfffff)
			return -EINVAL;
	}
	if (p != end)
		return -EINVAL;
	if (!c->type_cnt)
		return -ENODATA;

	c->type_offs = vmalloc(sizeof(u32) * c->type_cnt);
	if (!c->type_offs)
		return -ENOMEM;

	p = base + h.type_off;
	for (i = 0; i < c->type_cnt; i++) {
		const struct btf_type *t = (const struct btf_type *)p;

		c->type_offs[i] = p - (base + h.type_off);
		p += sizeof(*t) + btf_extra(BTF_INFO_KIND(t->info), BTF_INFO_VLEN(t->info));
	}
	return 0;
}

void btf_close(struct btf_cursor *c)
{
	vfree(c->type_offs);
	c->type_offs = NULL;
}

const struct btf_type *btf_type(const struct btf_cursor *c, u32 id)
{
	u32 i;

	if (!id || id <= c->id_off)
		return NULL;
	i = id - c->id_off - 1;
	if (i >= c->type_cnt)
		return NULL;
	return (const struct btf_type *)((const char *)c->blob + c->hdr_len +
					 c->type_off + c->type_offs[i]);
}

const char *btf_str(const struct btf_cursor *c, u32 off)
{
	if (off < c->str_base_off || off - c->str_base_off >= c->str_len)
		return NULL;
	return c->strs + (off - c->str_base_off);
}
