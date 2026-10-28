// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/stddef.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "reg.h"

static const struct ti_ctx *q_ctx;

static const struct btf_type *ctx_type(const struct ti_ctx *c, u32 id)
{
	if (!id)
		return NULL;
	if (id <= c->cur.id_off) {
		if (!c->base)
			return NULL;
		return ctx_type(c->base, id);
	}
	return btf_type(&c->cur, id);
}

static const struct ti_ctx *ctx_base_of(const struct ti_ctx *c)
{
	while (c->base)
		c = c->base;
	return c;
}

static const char *ctx_str(const struct ti_ctx *c, u32 off)
{
	const struct ti_ctx *b;
	const char *s;

	if (off < c->cur.str_base_off) {
		b = ctx_base_of(c);
		if (off >= b->cur.str_len)
			return "";
		s = btf_str(&b->cur, off);
	} else {
		s = btf_str(&c->cur, off);
	}
	return s ? s : "";
}

static int ns_cmp(const void *a, const void *b)
{
	const struct ti_nsent *x = a;
	const struct ti_nsent *y = b;

	return strcmp(ctx_str(q_ctx, x->name_off), ctx_str(q_ctx, y->name_off));
}

static int ns_key_cmp(const void *key, const void *elt)
{
	const char *name = key;
	const struct ti_nsent *e = elt;

	return strcmp(name, ctx_str(q_ctx, e->name_off));
}

static void ns_swap(struct ti_nsent *x, struct ti_nsent *y)
{
	struct ti_nsent t = *x;

	*x = *y;
	*y = t;
}

static void ns_sift(struct ti_nsent *a, u32 n, u32 root)
{
	for (;;) {
		u32 l = root * 2 + 1;
		u32 r = l + 1;
		u32 m = root;

		if (l < n && ns_cmp(&a[l], &a[m]) > 0)
			m = l;
		if (r < n && ns_cmp(&a[r], &a[m]) > 0)
			m = r;
		if (m == root)
			break;
		ns_swap(&a[root], &a[m]);
		root = m;
	}
}

static void ns_sort(struct ti_nsent *a, u32 n)
{
	u32 i;

	for (i = n / 2; i-- > 0;)
		ns_sift(a, n, i);
	for (i = n - 1; i > 0; i--) {
		ns_swap(&a[0], &a[i]);
		ns_sift(a, i, 0);
	}
}

static struct ti_nsent *ns_bsearch(const char *name, struct ti_nsent *a,
				   u32 n)
{
	u32 lo = 0;
	u32 hi = n;

	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		int c = ns_key_cmp(name, &a[mid]);

		if (!c)
			return &a[mid];
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

#define TI_STR_CAND	64

static void ti_str_vote(u32 n, u32 l, u32 *cands, u32 *cnts, u32 *nc)
{
	u64 d;
	u32 i;

	if (n <= l)
		return;
	d = (u64)n - l;
	if (d < (1 << 20) || d >= (1 << 30))
		return;
	for (i = 0; i < *nc; i++) {
		if (cands[i] == (u32)d) {
			cnts[i]++;
			return;
		}
	}
	if (*nc < TI_STR_CAND) {
		cands[*nc] = (u32)d;
		cnts[*nc] = 1;
		(*nc)++;
	}
}

/* module blob private strings reference the build-time base string table:
 * name_off = B + local_off. estimate B by voting name_off - local_off. */
static u32 ti_str_base_est(const struct ti_ctx *c)
{
	const struct btf_cursor *cur = &c->cur;
	u32 local[TI_STR_CAND];
	u32 cands[TI_STR_CAND];
	u32 cnts[TI_STR_CAND];
	u32 lcnt = 0;
	u32 nc = 0;
	u32 off = 0;
	u32 best = 0;
	u32 bestcnt = 0;
	u32 i;

	while (off < cur->str_len && lcnt < TI_STR_CAND) {
		local[lcnt++] = off;
		while (off < cur->str_len && cur->strs[off])
			off++;
		off++;
	}
	if (!lcnt)
		return 0;

	for (i = 1; i <= cur->type_cnt; i++) {
		const struct btf_type *t = btf_type(cur, cur->id_off + i);
		const struct btf_member *m;
		u32 kind;
		u32 vlen;
		u32 j;

		if (!t)
			break;
		for (j = 0; j < lcnt; j++)
			ti_str_vote(t->name_off, local[j], cands, cnts, &nc);
		kind = BTF_INFO_KIND(t->info);
		if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
			continue;
		vlen = BTF_INFO_VLEN(t->info);
		m = (const struct btf_member *)(t + 1);
		for (j = 0; j < vlen; j++) {
			u32 k;

			for (k = 0; k < lcnt; k++)
				ti_str_vote(m[j].name_off, local[k], cands,
					    cnts, &nc);
		}
	}
	for (i = 0; i < nc; i++) {
		if (cnts[i] > bestcnt) {
			bestcnt = cnts[i];
			best = cands[i];
		}
	}
	if (bestcnt < 2)
		return 0;
	return best;
}

static bool ti_root_is_vmlinux(const struct ti_ctx *c)
{
	const struct ti_ctx *b = c->base;

	if (!b)
		return false;
	while (b->base)
		b = b->base;
	return b == ti_base();
}

int ti_ctx_init(struct ti_ctx *c, const void *blob, u32 size)
{
	struct ti_nsent *map;
	u32 i;
	int ret;

	if (!c)
		return -EINVAL;

	ret = btf_open(blob, size, &c->cur);
	if (ret)
		return ret;

	if (c->base) {
		c->cur.id_off = c->base->cur.type_cnt;
		c->cur.str_base_off = c->base->cur.str_len;
		if (ti_root_is_vmlinux(c)) {
			u32 B = ti_str_base_est(c);

			pr_info("[type_info] %s base str_len=%u est=%u\n",
				c->name, c->cur.str_base_off, B);
			if (B && B != c->cur.str_base_off)
				c->cur.str_base_off = B;
		}
	}

	map = vmalloc(sizeof(*map) * c->cur.type_cnt);
	if (!map) {
		btf_close(&c->cur);
		return -ENOMEM;
	}
	for (i = 0; i < c->cur.type_cnt; i++) {
		const struct btf_type *t;

		t = btf_type(&c->cur, c->cur.id_off + 1 + i);
		map[i].name_off = t->name_off;
		map[i].id = c->cur.id_off + 1 + i;
	}

	q_ctx = c;
	ns_sort(map, c->cur.type_cnt);
	q_ctx = NULL;

	c->name_map = map;
	c->name_cnt = c->cur.type_cnt;
	return 0;
}

int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out)
{
	struct ti_ctx *c;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	ret = ti_ctx_init(c, blob, size);
	if (ret) {
		kfree(c);
		return ret;
	}
	*out = c;
	return 0;
}

void ti_ctx_close(struct ti_ctx *ctx)
{
	if (!ctx)
		return;
	vfree(ctx->name_map);
	btf_close(&ctx->cur);
#ifdef CONFIG_TI_DWARF
	ti_dw_free(ctx->dw);
	kfree(ctx->dw);
#endif
	kfree(ctx);
}

static int ti_name_own(const struct ti_ctx *ctx, const char *name,
		       u32 kind_mask, u32 *out)
{
	struct ti_nsent *hit;
	u32 i;

	q_ctx = ctx;
	hit = ns_bsearch(name, ctx->name_map, ctx->name_cnt);
	q_ctx = NULL;
	if (!hit)
		return -ENOENT;
	i = hit - ctx->name_map;
	while (i > 0 &&
	       !strcmp(name, ctx_str(ctx, ctx->name_map[i - 1].name_off)))
		i--;
	for (; i < ctx->name_cnt &&
	       !strcmp(name, ctx_str(ctx, ctx->name_map[i].name_off)); i++) {
		const struct btf_type *t;

		t = btf_type(&ctx->cur, ctx->name_map[i].id);
		if (kind_mask && !(kind_mask & BIT(BTF_INFO_KIND(t->info))))
			continue;
		*out = ctx->name_map[i].id;
		return 0;
	}
	return -ENOENT;
}

int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
		    u32 kind_mask, u32 *out)
{
	int ret;
	int idx;

	if (!ctx || !name || !out)
		return -EINVAL;

	ret = ti_name_own(ctx, name, kind_mask, out);
	if (!ret)
		return 0;

#ifdef CONFIG_TI_REMAP
	{
		const struct ti_ctx *kb = ti_base();
		const struct ti_ctx *c;
		u32 bid;
		u32 mid;
		int br;

		/* kernel reference is always vmlinux. remap only when the
		 * ctx base chain tops out at vmlinux: its type ids stay
		 * valid in this ctx (distilled base ctx ids do not) */
		c = ctx;
		while (c->base)
			c = c->base;
		if (c == kb && kb->cur.type_cnt) {
			br = ti_name_own(kb, name, kind_mask, &bid);
			if (!br) {
				/* module blob also defines it: keep own
				 * unless the definitions match in size */
				if (!ti_name_own(ctx, name, kind_mask, &mid) &&
				    ti_type_size(ctx, mid) !=
					    ti_type_size(kb, bid))
					return -ENOENT;
				*out = bid;
				return 0;
			}
		}
	}
#endif

	if (kind_mask && !(kind_mask & BIT(BTF_KIND_STRUCT)))
		return -ENOENT;
	idx = ti_reg_lookup(name);
	if (idx < 0)
		return -ENOENT;
	*out = ti_reg_idx_to_id(idx);
	return 0;
}

u32 ti_type_size(const struct ti_ctx *ctx, u32 id)
{
	const struct btf_type *t;
	u32 kind;
	int idx;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_size(idx);

	t = ctx_type(ctx, id);
	if (!t)
		return 0;
	kind = BTF_INFO_KIND(t->info);

	switch (kind) {
	case BTF_KIND_PTR:
		return sizeof(void *);
	case BTF_KIND_ARRAY: {
		const struct btf_array *a = (const struct btf_array *)(t + 1);

		return ti_type_size(ctx, a->type) * a->nelems;
	}
	default:
		return t->size;
	}
}

int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out)
{
	u32 cur = id;
	int depth = 0;

	if (ti_reg_id_to_idx(id) >= 0) {
		*out = id;
		return 0;
	}

	while (cur) {
		const struct btf_type *t = ctx_type(ctx, cur);
		u32 kind;

		if (!t)
			return -ENOENT;
		kind = BTF_INFO_KIND(t->info);
		if (kind == BTF_KIND_TYPEDEF ||
		    (BTF_KIND_QUAL & BIT(kind))) {
			cur = t->type;
			depth++;
			if (depth > 64)
				return -ELOOP;
			continue;
		}
		break;
	}
	*out = cur;
	return 0;
}

static int ti_member_flat_count(const struct ti_ctx *ctx, u32 id, int depth)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	u32 n = 0;
	u32 i;

	if (depth > 8)
		return 0;
	if (ti_follow(ctx, id, &id))
		return 0;
	t = ctx_type(ctx, id);
	if (!t)
		return 0;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return 0;
	vlen = BTF_INFO_VLEN(t->info);
	m = (const struct btf_member *)(t + 1);
	for (i = 0; i < vlen; i++) {
		const struct btf_type *mt;
		u32 mttype;

		mttype = m[i].type;
		mt = ctx_type(ctx, mttype);
		if (mt && !ctx_str(ctx, m[i].name_off)[0] &&
		    (BTF_INFO_KIND(mt->info) == BTF_KIND_STRUCT ||
		     BTF_INFO_KIND(mt->info) == BTF_KIND_UNION)) {
			u32 cn = ti_member_flat_count(ctx, mttype, depth + 1);

			if (cn)
				n += cn;
			else
				n++;
			continue;
		}
		n++;
	}
	return n;
}

static int ti_member_flat_at(const struct ti_ctx *ctx, u32 id, u32 *widx,
			     u32 target, const char **name, u32 *type,
			     u32 *bit_off, u32 *bit_sz, int depth)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	u32 i;

	if (depth > 8)
		return -ELOOP;
	if (ti_follow(ctx, id, &id))
		return -ENOENT;
	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;
	vlen = BTF_INFO_VLEN(t->info);
	m = (const struct btf_member *)(t + 1);
	for (i = 0; i < vlen; i++) {
		const struct btf_type *mt;
		u32 mttype;
		u32 base_off;
		u32 base_sz;

		if (BTF_INFO_KFLAG(t->info)) {
			base_off = BTF_MEMBER_BIT_OFF(m[i].offset);
			base_sz = BTF_MEMBER_BIT_SZ(m[i].offset);
		} else {
			base_off = m[i].offset;
			base_sz = 0;
		}
		mttype = m[i].type;
		mt = ctx_type(ctx, mttype);
		if (mt && !ctx_str(ctx, m[i].name_off)[0] &&
		    (BTF_INFO_KIND(mt->info) == BTF_KIND_STRUCT ||
		     BTF_INFO_KIND(mt->info) == BTF_KIND_UNION) &&
		    ti_member_flat_count(ctx, mttype, depth + 1)) {
			int ret = ti_member_flat_at(ctx, mttype, widx, target,
						    name, type, bit_off,
						    bit_sz, depth + 1);

			if (!ret) {
				*bit_off += base_off;
				return 0;
			}
			if (ret != -ENOENT)
				return ret;
			continue;
		}
		if (*widx == target) {
			if (name)
				*name = ctx_str(ctx, m[i].name_off);
			if (type)
				*type = mttype;
			*bit_off = base_off;
			*bit_sz = base_sz;
			return 0;
		}
		(*widx)++;
	}
	return -ENOENT;
}

#ifdef CONFIG_TI_FEATURE
static int ti_scan_feature(const struct ti_ctx *ctx, u32 size, u32 vlen,
			   const struct ti_member_desc *seq, u32 n,
			   u32 *out)
{
	u32 ranges[2][2];
	u32 r;
	u32 i;

	/* own types first, then base */
	ranges[0][0] = ctx->cur.id_off + 1;
	ranges[0][1] = ctx->cur.id_off + ctx->cur.type_cnt;
	ranges[1][0] = 1;
	ranges[1][1] = ctx->cur.id_off;
	for (r = 0; r < 2; r++) {
		for (i = ranges[r][0]; i <= ranges[r][1]; i++) {
			const struct btf_type *t = ctx_type(ctx, i);
			u32 kind;
			u32 vlen_t;
			const struct btf_member *m;
			u32 j;

			if (!t)
				break;
			kind = BTF_INFO_KIND(t->info);
			if (kind != BTF_KIND_STRUCT &&
			    kind != BTF_KIND_UNION)
				continue;
			if (t->size != size)
				continue;
			vlen_t = BTF_INFO_VLEN(t->info);
			if (vlen && vlen_t != vlen)
				continue;
			if (n) {
				if (vlen_t < n)
					continue;
				m = (const struct btf_member *)(t + 1);
				for (j = 0; j < n; j++) {
					u32 bo;
					u32 bs;

					if (BTF_INFO_KFLAG(t->info)) {
						bo = BTF_MEMBER_BIT_OFF(
							m[j].offset);
						bs = BTF_MEMBER_BIT_SZ(
							m[j].offset);
					} else {
						bo = m[j].offset;
						bs = 0;
					}
					if (bo != seq[j].bit_off ||
					    bs != seq[j].bit_sz)
						break;
					if (seq[j].name &&
					    strcmp(ctx_str(ctx,
							   m[j].name_off),
						   seq[j].name))
						break;
				}
				if (j != n)
					continue;
			}
			*out = i;
			return 0;
		}
	}
	return -ENOENT;
}

int ti_type_by_size(const struct ti_ctx *ctx, u32 size, u32 vlen,
		    u32 *out)
{
	if (!ctx || !out || !size)
		return -EINVAL;
	return ti_scan_feature(ctx, size, vlen, NULL, 0, out);
}

int ti_type_by_seq(const struct ti_ctx *ctx, u32 size,
		   const struct ti_member_desc *seq, u32 n, u32 *out)
{
	if (!ctx || !out || !size || !n || !seq)
		return -EINVAL;
	return ti_scan_feature(ctx, size, 0, seq, n, out);
}
#endif

int ti_member_count(const struct ti_ctx *ctx, u32 id)
{
	int idx;

	if (!ctx)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_mem_count(idx);

	return ti_member_flat_count(ctx, id, 0);
}

int ti_member_at(const struct ti_ctx *ctx, u32 id, u32 idx,
		 const char **name, u32 *type, u32 *bit_off, u32 *bit_sz)
{
	u32 widx = 0;
	int ridx;

	if (!ctx || !bit_off || !bit_sz)
		return -EINVAL;

	ridx = ti_reg_id_to_idx(id);
	if (ridx >= 0) {
		const char *mn = NULL;
		int r;

		if (name)
			r = ti_reg_mem_at(ridx, idx, name, bit_off, bit_sz);
		else
			r = ti_reg_mem_at(ridx, idx, &mn, bit_off, bit_sz);
		if (!r && type)
			*type = 0;
		return r;
	}

	{
		int r = ti_member_flat_at(ctx, id, &widx, idx, name, type,
					  bit_off, bit_sz, 0);

		if (r)
			return r;
#ifdef CONFIG_TI_DWARF
		if (ctx->dw && name) {
			const struct btf_type *t;
			const char *sname;
			const char *dn;
			u32 fid = id;

			if (!ti_follow(ctx, fid, &fid))
				t = ctx_type(ctx, fid);
			else
				t = NULL;
			if (t) {
				sname = ctx_str(ctx, t->name_off);
				dn = ti_dw_member_name(ctx->dw, sname,
						       *bit_off, *bit_sz);
				if (dn)
					*name = dn;
			}
		}
#endif
		return 0;
	}
}

static int ti_member_lookup(const struct ti_ctx *ctx, u32 id,
			    const char *member, u32 *type_out, u32 *bit_off,
			    u32 *bit_sz, int depth)
{
	const struct btf_type *t;
	const struct btf_member *m;
	u32 kind;
	u32 vlen;
	u32 i;
	int ret;

	if (depth > 8)
		return -ELOOP;

	ret = ti_follow(ctx, id, &id);
	if (ret)
		return ret;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
		return -EINVAL;

	vlen = BTF_INFO_VLEN(t->info);
	m = (const struct btf_member *)(t + 1);
	for (i = 0; i < vlen; i++) {
		if (strcmp(ctx_str(ctx, m[i].name_off), member))
			continue;
		if (BTF_INFO_KFLAG(t->info)) {
			*bit_off = BTF_MEMBER_BIT_OFF(m[i].offset);
			*bit_sz = BTF_MEMBER_BIT_SZ(m[i].offset);
		} else {
			*bit_off = m[i].offset;
			*bit_sz = 0;
		}
		if (type_out)
			*type_out = m[i].type;
		return 0;
	}

	/* anonymous struct/union member wraps the fields (RANDSTRUCT),
	 * recurse into it */
	for (i = 0; i < vlen; i++) {
		const struct btf_type *mt;
		u32 toff;
		u32 tsz;
		u32 ttype;

		if (ctx_str(ctx, m[i].name_off)[0])
			continue;
		ttype = m[i].type;
		mt = ctx_type(ctx, ttype);
		if (!mt)
			continue;
		kind = BTF_INFO_KIND(mt->info);
		if (kind != BTF_KIND_STRUCT && kind != BTF_KIND_UNION)
			continue;
		ret = ti_member_lookup(ctx, ttype, member, NULL, &toff, &tsz,
				       depth + 1);
		if (ret)
			continue;
		if (BTF_INFO_KFLAG(t->info)) {
			*bit_off = BTF_MEMBER_BIT_OFF(m[i].offset) + toff;
			*bit_sz = BTF_MEMBER_BIT_SZ(m[i].offset);
		} else {
			*bit_off = m[i].offset + toff;
			*bit_sz = tsz;
		}
		if (type_out)
			*type_out = ttype;
		return 0;
	}

#ifdef CONFIG_TI_DWARF
	/* DWARF member table: names are always valid, offsets come from
	 * the module's own compile-time layout */
	if (ctx->dw) {
		const char *sname = ctx_str(ctx, t->name_off);
		u32 d_off;
		u32 d_sz;

		if (!ti_dw_member_off(ctx->dw, sname, member, &d_off, &d_sz)) {
			*bit_off = d_off;
			*bit_sz = d_sz;
			if (type_out)
				*type_out = 0;
			return 0;
		}
	}
#endif
	return -ENOENT;
}

int ti_member_off(const struct ti_ctx *ctx, u32 id, const char *member,
		  u32 *bit_off, u32 *bit_sz)
{
	int idx;

	if (!ctx || !member || !bit_off || !bit_sz)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0)
		return ti_reg_member(idx, member, bit_off, bit_sz);

	return ti_member_lookup(ctx, id, member, NULL, bit_off, bit_sz, 0);
}

int ti_member_info(const struct ti_ctx *ctx, u32 id, const char *member,
		   u32 *type, u32 *bit_off, u32 *bit_sz)
{
	int idx;

	if (!ctx || !member || !type || !bit_off || !bit_sz)
		return -EINVAL;

	idx = ti_reg_id_to_idx(id);
	if (idx >= 0) {
		*type = 0;
		return ti_reg_member(idx, member, bit_off, bit_sz);
	}

	return ti_member_lookup(ctx, id, member, type, bit_off, bit_sz, 0);
}

#ifdef CONFIG_TI_FUNC

static const struct btf_cursor *ctx_cur_own(const struct ti_ctx *c, u32 id)
{
	if (id <= c->cur.id_off) {
		if (!c->base)
			return NULL;
		return ctx_cur_own(c->base, id);
	}
	return &c->cur;
}

static int ti_func_proto_id(const struct ti_ctx *ctx, u32 id, u32 *proto_id)
{
	const struct btf_type *t;
	u32 kind;

	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	if (kind == BTF_KIND_FUNC_PROTO) {
		*proto_id = id;
		return 0;
	}
	if (kind != BTF_KIND_FUNC)
		return -EINVAL;
	*proto_id = t->type;
	t = ctx_type(ctx, *proto_id);
	if (!t || BTF_INFO_KIND(t->info) != BTF_KIND_FUNC_PROTO)
		return -EINVAL;
	return 0;
}

int ti_func_proto(const struct ti_ctx *ctx, u32 func_id, u32 *ret_type,
		  u32 *nparams)
{
	const struct btf_type *t;
	u32 pid;
	int ret;

	if (!ctx || !ret_type || !nparams)
		return -EINVAL;
	ret = ti_func_proto_id(ctx, func_id, &pid);
	if (ret)
		return ret;
	t = ctx_type(ctx, pid);
	*ret_type = t->type;
	*nparams = BTF_INFO_VLEN(t->info);
	return 0;
}

int ti_func_param(const struct ti_ctx *ctx, u32 func_id, u32 idx,
		  const char **name, u32 *type)
{
	const struct btf_type *t;
	const struct btf_param *p;
	u32 pid;
	u32 vlen;
	int ret;

	if (!ctx)
		return -EINVAL;
	ret = ti_func_proto_id(ctx, func_id, &pid);
	if (ret)
		return ret;
	t = ctx_type(ctx, pid);
	vlen = BTF_INFO_VLEN(t->info);
	if (idx >= vlen)
		return -ENOENT;
	p = (const struct btf_param *)(t + 1);
	if (name)
		*name = ctx_str(ctx, p[idx].name_off);
	if (type)
		*type = p[idx].type;
	return 0;
}

int ti_enum_at(const struct ti_ctx *ctx, u32 id, u32 idx,
	       const char **name, s64 *val)
{
	const struct btf_type *t;
	u32 kind;
	u32 vlen;

	if (!ctx)
		return -EINVAL;
	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	vlen = BTF_INFO_VLEN(t->info);
	if (kind == BTF_KIND_ENUM64) {
		const struct btf_enum64 *e = (const struct btf_enum64 *)(t + 1);

		if (idx >= vlen)
			return -ENOENT;
		if (name)
			*name = ctx_str(ctx, e[idx].name_off);
		if (val)
			*val = (s64)((u64)e[idx].val_hi32 << 32 |
				    e[idx].val_lo32);
		return 0;
	}
	if (kind != BTF_KIND_ENUM)
		return -EINVAL;
	{
		const struct btf_enum *e = (const struct btf_enum *)(t + 1);

		if (idx >= vlen)
			return -ENOENT;
		if (name)
			*name = ctx_str(ctx, e[idx].name_off);
		if (val)
			*val = e[idx].val;
	}
	return 0;
}

int ti_enum_val(const struct ti_ctx *ctx, u32 id, const char *name,
		s64 *val)
{
	u32 i;

	if (!ctx || !name || !val)
		return -EINVAL;
	if (!ctx_type(ctx, id))
		return -ENOENT;
	for (i = 0;; i++) {
		const char *mn;
		s64 mv;

		if (ti_enum_at(ctx, id, i, &mn, &mv))
			break;
		if (!strcmp(name, mn)) {
			*val = mv;
			return 0;
		}
	}
	return -ENOENT;
}

int ti_type_raw(const struct ti_ctx *ctx, u32 id, struct ti_type_raw *out)
{
	const struct btf_cursor *cur;
	const struct btf_type *t;
	const u8 *end;
	u32 kind;

	if (!ctx || !out)
		return -EINVAL;
	t = ctx_type(ctx, id);
	if (!t)
		return -ENOENT;
	cur = ctx_cur_own(ctx, id);
	if (!cur)
		return -ENOENT;
	kind = BTF_INFO_KIND(t->info);
	out->kind = kind;
	out->vlen = BTF_INFO_VLEN(t->info);
	out->type = t->type;
	out->name = ctx_str(ctx, t->name_off);
	out->data = (const void *)(t + 1);
	end = (const u8 *)cur->blob + cur->hdr_len + cur->type_off +
	      cur->type_len;
	out->data_len = (const u8 *)out->data < end ?
			(u32)(end - (const u8 *)out->data) : 0;
	return 0;
}
#endif
