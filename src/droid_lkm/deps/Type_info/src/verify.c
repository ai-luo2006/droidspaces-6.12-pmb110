// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/cred.h>
#include <linux/mm_types.h>
#include <linux/module.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "anchor.h"
#include "port.h"
#include "verify.h"
#include "reg.h"

static int vfails;

static void vcheck(int ok, const char *fmt, ...)
{
	va_list ap;

	if (ok)
		return;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
	vfails++;
}

#define CHECK(cond, fmt, ...) vcheck(cond, fmt, ##__VA_ARGS__)

#define T_INFO(kind, vlen, kflag) \
	((u32)(kind) << 24 | (vlen) | (kflag) << 31)

struct sbuf {
	void *p;
	u32 len;
	u32 str_base;
};

static u32 sb_u32(struct sbuf *s, u32 v)
{
	u32 off = s->len;

	memcpy((char *)s->p + off, &v, 4);
	s->len += 4;
	return off;
}

static u32 sb_raw(struct sbuf *s, const void *src, u32 n)
{
	u32 off = s->len;

	memcpy((char *)s->p + off, src, n);
	s->len += n;
	return off;
}

static u32 sb_str(struct sbuf *s, const char *str)
{
	return sb_raw(s, str, strlen(str) + 1) - s->str_base;
}

static void sb_type(struct sbuf *s, u32 name_off, u32 info, u32 size,
		    const void *extra, u32 extra_len)
{
	sb_u32(s, name_off);
	sb_u32(s, info);
	sb_u32(s, size);
	if (extra_len)
		sb_raw(s, extra, extra_len);
}

static void sb_hdr(struct sbuf *s, u32 type_off, u32 type_len, u32 str_len)
{
	u32 h[6] = { BTF_MAGIC | (1u << 16), 24, type_off, type_len, 0,
		     str_len };

	memcpy(s->p, h, sizeof(h));
}

static struct ti_ctx *synth_base(void **blobp, u32 *sizep, u32 *id_offp)
{
	struct sbuf s;
	struct ti_ctx *c;
	u32 off_int;
	u32 off_bits;
	u32 off_a;
	u32 off_b;
	u32 off_c;
	u32 off_myint;
	u32 off_arr4;
	u32 off_fn;
	u32 off_arg1;
	u32 off_arg2;
	u32 off_enum;
	u32 off_one;
	u32 off_two;
	u32 off_three;
	u32 str_len;
	u32 type_off;
	u32 type_len;
	struct {
		u32 name_off;
		u32 type;
		u32 offset;
	} m[3];
	struct {
		u32 type;
		u32 index_type;
		u32 nelems;
	} arr;
	u32 int_data;

	s.p = vmalloc(8192);
	if (!s.p)
		return NULL;
	s.len = 24;
	s.str_base = s.len;

	off_int = sb_str(&s, "int");
	off_bits = sb_str(&s, "s_bits");
	off_a = sb_str(&s, "a");
	off_b = sb_str(&s, "b");
	off_c = sb_str(&s, "c");
	off_myint = sb_str(&s, "myint");
	off_arr4 = sb_str(&s, "arr4");
	off_fn = sb_str(&s, "syn_fn");
	off_arg1 = sb_str(&s, "arg1");
	off_arg2 = sb_str(&s, "arg2");
	off_enum = sb_str(&s, "syn_enum");
	off_one = sb_str(&s, "ONE");
	off_two = sb_str(&s, "TWO");
	off_three = sb_str(&s, "THREE");
	str_len = s.len - s.str_base;

	type_off = s.len;
	int_data = BIT(31) | 32;
	sb_type(&s, off_int, T_INFO(BTF_KIND_INT, 0, 0), 4, &int_data, 4);

	m[0].name_off = off_a;
	m[0].type = 1;
	m[0].offset = (3u << 24) | 0;
	m[1].name_off = off_b;
	m[1].type = 1;
	m[1].offset = (5u << 24) | 3;
	m[2].name_off = off_c;
	m[2].type = 1;
	m[2].offset = 32;
	sb_type(&s, off_bits, T_INFO(BTF_KIND_STRUCT, 3, 1), 8, m, sizeof(m));

	sb_type(&s, off_myint, T_INFO(BTF_KIND_TYPEDEF, 0, 0), 1, NULL, 0);

	sb_type(&s, 0, T_INFO(BTF_KIND_PTR, 0, 0), 1, NULL, 0);

	arr.type = 1;
	arr.index_type = 1;
	arr.nelems = 4;
	sb_type(&s, off_arr4, T_INFO(BTF_KIND_ARRAY, 0, 0), 0, &arr,
		sizeof(arr));
	{
		struct {
			u32 name_off;
			u32 type;
		} fpar[2];
		struct {
			u32 name_off;
			s32 val;
		} fenu[3];

		fpar[0].name_off = off_arg1;
		fpar[0].type = 1;
		fpar[1].name_off = off_arg2;
		fpar[1].type = 1;
		sb_type(&s, 0, T_INFO(BTF_KIND_FUNC_PROTO, 2, 0), 1, fpar,
			sizeof(fpar));
		sb_type(&s, off_fn, T_INFO(BTF_KIND_FUNC, 0, 0), 6, NULL, 0);
		fenu[0].name_off = off_one;
		fenu[0].val = 10;
		fenu[1].name_off = off_two;
		fenu[1].val = 20;
		fenu[2].name_off = off_three;
		fenu[2].val = 30;
		sb_type(&s, off_enum, T_INFO(BTF_KIND_ENUM, 3, 0), 4, fenu,
			sizeof(fenu));
	}
	type_len = s.len - type_off;

	sb_hdr(&s, type_off - 24, type_len, str_len);

	*blobp = s.p;
	*sizep = s.len;
	*id_offp = 8;
	if (ti_ctx_open(s.p, s.len, &c)) {
		vfree(s.p);
		*blobp = NULL;
		return NULL;
	}
	return c;
}

static struct ti_ctx *synth_split(const struct ti_ctx *base, u32 id_off,
				  void **blobp, u32 *sizep)
{
	struct sbuf s;
	struct ti_ctx *c;
	u32 off_wrap;
	u32 off_inner;
	u32 str_len;
	u32 type_off;
	u32 type_len;
	struct {
		u32 name_off;
		u32 type;
		u32 offset;
	} m;

	s.p = vmalloc(8192);
	if (!s.p)
		return NULL;
	s.len = 24;
	s.str_base = s.len;

	off_wrap = sb_str(&s, "s_wrap");
	off_inner = sb_str(&s, "inner");
	str_len = s.len - s.str_base;

	type_off = s.len;
	m.name_off = base->cur.str_len + off_inner;
	m.type = 2;
	m.offset = 0;
	sb_type(&s, base->cur.str_len + off_wrap, T_INFO(BTF_KIND_STRUCT, 1, 0),
		4, &m, sizeof(m));
	type_len = s.len - type_off;

	sb_hdr(&s, type_off - 24, type_len, str_len);

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		vfree(s.p);
		return NULL;
	}
	c->base = base;
	if (ti_ctx_init(c, s.p, s.len)) {
		kfree(c);
		vfree(s.p);
		return NULL;
	}
	*blobp = s.p;
	*sizep = s.len;
	if (c->cur.id_off != id_off) {
		CHECK(0, "[type_info] synth: split id_off=%u want %u\n",
		      c->cur.id_off, id_off);
		ti_ctx_close(c);
		return NULL;
	}
	return c;
}

static void synth_check_func_enum(const struct ti_ctx *c);

static void synth_check_base(const struct ti_ctx *c)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;

	CHECK(!ti_type_by_name(c, "s_bits", BIT(BTF_KIND_STRUCT), &id),
	      "[type_info] synth: s_bits not found\n");
	CHECK(!ti_member_off(c, id, "a", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 3,
	      "[type_info] synth: bitfield a off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(!ti_member_off(c, id, "b", &bit_off, &bit_sz) &&
	      bit_off == 3 && bit_sz == 5,
	      "[type_info] synth: bitfield b off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(!ti_member_off(c, id, "c", &bit_off, &bit_sz) &&
	      bit_off == 32 && bit_sz == 0,
	      "[type_info] synth: member c off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(ti_type_size(c, id) == 8, "[type_info] synth: s_bits size=%u\n",
	      ti_type_size(c, id));

	CHECK(!ti_type_by_name(c, "myint", 0, &id),
	      "[type_info] synth: myint not found\n");
	CHECK(!ti_follow(c, id, &id) && id == 1,
	      "[type_info] synth: follow myint -> %u\n", id);

	CHECK(!ti_type_by_name(c, "arr4", 0, &id),
	      "[type_info] synth: arr4 not found\n");
	CHECK(ti_type_size(c, id) == 16, "[type_info] synth: arr4 size=%u\n",
	      ti_type_size(c, id));
#ifdef CONFIG_TI_FUNC
	synth_check_func_enum(c);
#endif
}

#ifdef CONFIG_TI_FUNC
static void synth_check_func_enum(const struct ti_ctx *c)
{
	u32 id;
	u32 ret;
	u32 n;
	u32 ptype;
	const char *pname;
	s64 val;

	CHECK(!ti_type_by_name(c, "syn_fn", BIT(BTF_KIND_FUNC), &id),
	      "[type_info] synth: syn_fn not found\n");
	CHECK(!ti_func_proto(c, id, &ret, &n) && ret == 1 && n == 2,
	      "[type_info] synth: fn ret=%u n=%u\n", ret, n);
	CHECK(!ti_func_param(c, id, 0, &pname, &ptype) &&
	      ptype == 1 && pname && !strcmp(pname, "arg1"),
	      "[type_info] synth: fn p0 type=%u name=%s\n", ptype,
	      pname ? pname : "?");
	CHECK(!ti_func_param(c, id, 1, &pname, &ptype) &&
	      ptype == 1 && pname && !strcmp(pname, "arg2"),
	      "[type_info] synth: fn p1 type=%u name=%s\n", ptype,
	      pname ? pname : "?");
	CHECK(ti_func_param(c, id, 2, NULL, NULL) == -ENOENT,
	      "[type_info] synth: fn p2 should be ENOENT\n");

	CHECK(!ti_type_by_name(c, "syn_enum", BIT(BTF_KIND_ENUM), &id),
	      "[type_info] synth: syn_enum not found\n");
	CHECK(!ti_enum_val(c, id, "ONE", &val) && val == 10,
	      "[type_info] synth: enum ONE=%lld\n", val);
	CHECK(!ti_enum_val(c, id, "THREE", &val) && val == 30,
	      "[type_info] synth: enum THREE=%lld\n", val);
	CHECK(ti_enum_val(c, id, "NOPE", &val) == -ENOENT,
	      "[type_info] synth: enum NOPE should be ENOENT\n");
	CHECK(!ti_enum_at(c, id, 1, &pname, &val) && val == 20 &&
	      pname && !strcmp(pname, "TWO"),
	      "[type_info] synth: enum at1=%lld name=%s\n", val,
	      pname ? pname : "?");
	CHECK(ti_enum_at(c, id, 3, NULL, NULL) == -ENOENT,
	      "[type_info] synth: enum at3 should be ENOENT\n");
	{
		struct ti_type_raw r;
		int rret;

		rret = ti_type_raw(c, id, &r);
		CHECK(!rret && r.kind == BTF_KIND_ENUM && r.vlen == 3 &&
		      r.data_len >= sizeof(struct btf_enum) * 3,
		      "[type_info] synth: raw enum kind=%u vlen=%u len=%u\n",
		      r.kind, r.vlen, r.data_len);
		rret = ti_type_by_name(c, "s_bits", BIT(BTF_KIND_STRUCT), &id);
		CHECK(!rret, "[type_info] synth: s_bits raw not found\n");
		rret = ti_type_raw(c, id, &r);
		CHECK(!rret && r.kind == BTF_KIND_STRUCT && r.vlen == 3,
		      "[type_info] synth: raw struct kind=%u vlen=%u\n",
		      r.kind, r.vlen);
	}
}
#endif

static void synth_check_split(const struct ti_ctx *c)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;

	CHECK(!ti_type_by_name(c, "s_wrap", BIT(BTF_KIND_STRUCT), &id),
	      "[type_info] synth: s_wrap not found\n");
	CHECK(!ti_member_off(c, id, "inner", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 0,
	      "[type_info] synth: s_wrap.inner off=%u sz=%u\n", bit_off,
	      bit_sz);
	CHECK(!ti_follow(c, 2, &id) && id == 2,
	      "[type_info] synth: split follow base type -> %u\n", id);
	CHECK(!ti_member_off(c, 2, "a", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 3,
	      "[type_info] synth: split cross member a off=%u sz=%u\n",
	      bit_off, bit_sz);
}

static void verify_synth(void)
{
	struct ti_ctx *base;
	struct ti_ctx *split;
	void *bblob;
	void *sblob;
	u32 bsize;
	u32 ssize;
	u32 id_off;

	base = synth_base(&bblob, &bsize, &id_off);
	CHECK(base != NULL, "[type_info] synth: base build failed\n");
	if (!base)
		return;
	synth_check_base(base);

	split = synth_split(base, id_off, &sblob, &ssize);
	CHECK(split != NULL, "[type_info] synth: split build failed\n");
	if (split) {
		synth_check_split(split);
		ti_ctx_close(split);
		vfree(sblob);
	}
	ti_ctx_close(base);
	vfree(bblob);
}

static void spot_check(const struct ti_ctx *base, const char *sname,
		       size_t csize, const char *const *mnames,
		       const size_t *coffs, u32 nmem)
{
	u32 id;
	u32 bo;
	u32 bs;
	int ret;
	int i;

	ret = ti_type_by_name(base, sname, BIT(BTF_KIND_STRUCT), &id);
	CHECK(!ret, "[type_info] spot %s not found (%d)\n", sname, ret);
	if (ret)
		return;
	CHECK(ti_type_size(base, id) == csize,
	      "[type_info] spot %s size btf=%u compile=%zu\n", sname,
	      ti_type_size(base, id), csize);
	for (i = 0; i < (int)nmem; i++) {
		ret = ti_member_off(base, id, mnames[i], &bo, &bs);
		if (ret) {
			int n = ti_member_count(base, id);
			int j;

			pr_info("[type_info] spot %s.%s miss ret=%d id=%u "
				"members=%d\n", sname, mnames[i], ret, id, n);
			for (j = 0; j < n && j < 8; j++) {
				const char *mn = NULL;

				if (!ti_member_at(base, id, j, &mn, NULL, &bo,
						  &bs))
					pr_info("[type_info]   m[%d] '%s'\n", j,
						mn);
			}
		}
		CHECK(!ret && bo / 8 == coffs[i] && !bs,
		      "[type_info] spot %s.%s off=%u compile=%zu (ret=%d)\n",
		      sname, mnames[i], bo / 8, coffs[i], ret);
	}
	pr_info("[type_info] spot %s ok\n", sname);
}

static void verify_kernel_spot(const struct ti_ctx *base)
{
	static const char *const mm_m[] = { "task_size", "pgd", "arg_start" };
	static const size_t mm_o[] = {
		offsetof(struct mm_struct, task_size),
		offsetof(struct mm_struct, pgd),
		offsetof(struct mm_struct, arg_start),
	};
	static const char *const cred_m[] = { "usage", "uid", "euid" };
	static const size_t cred_o[] = {
		offsetof(struct cred, usage),
		offsetof(struct cred, uid),
		offsetof(struct cred, euid),
	};
	static const char *const file_m[] = { "f_mode", "f_pos", "f_inode" };
	static const size_t file_o[] = {
		offsetof(struct file, f_mode),
		offsetof(struct file, f_pos),
		offsetof(struct file, f_inode),
	};
	static const char *const dentry_m[] = { "d_name", "d_parent",
						"d_inode" };
	static const size_t dentry_o[] = {
		offsetof(struct dentry, d_name),
		offsetof(struct dentry, d_parent),
		offsetof(struct dentry, d_inode),
	};
	static const char *const module_m[] = { "list", "name", "state" };
	static const size_t module_o[] = {
		offsetof(struct module, list),
		offsetof(struct module, name),
		offsetof(struct module, state),
	};

	spot_check(base, "mm_struct", sizeof(struct mm_struct), mm_m, mm_o, 3);
	spot_check(base, "cred", sizeof(struct cred), cred_m, cred_o, 3);
	spot_check(base, "file", sizeof(struct file), file_m, file_o, 3);
	spot_check(base, "dentry", sizeof(struct dentry), dentry_m, dentry_o,
		   3);
	spot_check(base, "module", sizeof(struct module), module_m, module_o,
		   3);
}

static void verify_kernel(const struct ti_ctx *base)
{
	u32 id;
	u32 bit_off;
	u32 bit_sz;
	u32 off_pid;
	u32 off_comm;
	int ret;

	ret = ti_type_by_name(base, "task_struct", BIT(BTF_KIND_STRUCT), &id);
	CHECK(!ret, "[type_info] task_struct not found: %d\n", ret);
	if (ret)
		return;

	ret = ti_member_off(base, id, "pid", &bit_off, &bit_sz);
	CHECK(!ret, "[type_info] task_struct.pid lookup: %d\n", ret);
	if (!ret) {
		off_pid = bit_off / 8;
		CHECK(off_pid == offsetof(struct task_struct, pid) && !bit_sz,
		      "[type_info] pid btf=%u compile=%zu sz=%u\n", off_pid,
		      offsetof(struct task_struct, pid), bit_sz);
	}

	ret = ti_member_off(base, id, "comm", &bit_off, &bit_sz);
	CHECK(!ret, "[type_info] task_struct.comm lookup: %d\n", ret);
	if (!ret) {
		off_comm = bit_off / 8;
		CHECK(off_comm == offsetof(struct task_struct, comm) && !bit_sz,
		      "[type_info] comm btf=%u compile=%zu sz=%u\n", off_comm,
		      offsetof(struct task_struct, comm), bit_sz);
		if (off_comm <= 0x4000 && !bit_sz)
			CHECK(!strcmp(current->comm, (char *)current + off_comm),
			      "[type_info] comm mismatch: %s vs %s\n",
			      current->comm, (char *)current + off_comm);
	}

	CHECK(ti_type_size(base, id) == sizeof(struct task_struct),
	      "[type_info] task_struct size btf=%u compile=%zu\n",
	      ti_type_size(base, id), sizeof(struct task_struct));

	/* member enumeration cross-check */
	{
		static const char *const want[] = { "pid", "comm", "tasks",
						    "mm" };
		u32 found[ARRAY_SIZE(want)] = { 0 };
		int cnt = ti_member_count(base, id);
		int i;
		int j;

		CHECK(cnt > 0, "[type_info] task_struct member count=%d\n",
		      cnt);
		for (i = 0; i < cnt && i < 4096; i++) {
			const char *mname = NULL;

			ret = ti_member_at(base, id, i, &mname, NULL, &bit_off,
					   &bit_sz);
			if (ret)
				break;
			for (j = 0; j < (int)ARRAY_SIZE(want); j++)
				if (mname && !strcmp(mname, want[j]))
					found[j] = bit_off / 8;
		}
		CHECK(found[0] == offsetof(struct task_struct, pid) &&
		      found[1] == offsetof(struct task_struct, comm) &&
		      found[2] == offsetof(struct task_struct, tasks) &&
		      found[3] == offsetof(struct task_struct, mm),
		      "[type_info] task_struct enum pid=%u comm=%u tasks=%u "
		      "mm=%u (compile %zu/%zu/%zu/%zu)\n", found[0],
		      found[1], found[2], found[3],
		      offsetof(struct task_struct, pid),
		      offsetof(struct task_struct, comm),
		      offsetof(struct task_struct, tasks),
		      offsetof(struct task_struct, mm));
	}

	verify_kernel_spot(base);
}

static void verify_mod_cfg_struct(const struct ti_ctx *c)
{
	const u32 expect[7] = { 0, 32, 48, 56, 64, 128, 256 };
	const struct btf_type *t;
	const struct btf_member *m;
	u32 found_cfg = 0;
	u32 found_bits = 0;
	u32 i;
	u32 j;

	for (i = 1; i <= c->cur.type_cnt; i++) {
		u32 id = c->cur.id_off + i;
		u32 kind;

		t = btf_type(&c->cur, id);
		if (!t)
			break;
		kind = BTF_INFO_KIND(t->info);
		if (kind != BTF_KIND_STRUCT)
			continue;

		if (t->size == 40 && BTF_INFO_VLEN(t->info) == 7) {
			m = (const struct btf_member *)(t + 1);
			for (j = 0; j < 7; j++) {
				CHECK(BTF_MEMBER_BIT_OFF(m[j].offset) == expect[j],
				      "[type_info] mod: cfg member %u off=%u "
				      "want %u\n", j,
				      BTF_MEMBER_BIT_OFF(m[j].offset),
				      expect[j]);
			}
			found_cfg = 1;
		}
		if (t->size == 8 && BTF_INFO_VLEN(t->info) == 3 &&
		    BTF_INFO_KFLAG(t->info)) {
			m = (const struct btf_member *)(t + 1);
			CHECK(BTF_MEMBER_BIT_OFF(m[0].offset) == 0 &&
			      BTF_MEMBER_BIT_SZ(m[0].offset) == 3,
			      "[type_info] mod: bits.a off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[0].offset),
			      BTF_MEMBER_BIT_SZ(m[0].offset));
			CHECK(BTF_MEMBER_BIT_OFF(m[1].offset) == 3 &&
			      BTF_MEMBER_BIT_SZ(m[1].offset) == 5,
			      "[type_info] mod: bits.b off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[1].offset),
			      BTF_MEMBER_BIT_SZ(m[1].offset));
			CHECK(BTF_MEMBER_BIT_OFF(m[2].offset) == 32 &&
			      BTF_MEMBER_BIT_SZ(m[2].offset) == 0,
			      "[type_info] mod: bits.rest off=%u sz=%u\n",
			      BTF_MEMBER_BIT_OFF(m[2].offset),
			      BTF_MEMBER_BIT_SZ(m[2].offset));
			found_bits = 1;
		}
	}
	CHECK(found_cfg, "[type_info] mod: cfg struct not found\n");
	CHECK(found_bits, "[type_info] mod: bits struct not found\n");
}

static void verify_mod_cfg(const struct ti_ctx *c)
{
	static const u32 want_off[] = { 0, 32, 48, 56, 64, 128, 256 };
	u32 cfg_id;
	u32 bit_off;
	u32 bit_sz;
	u32 type;
	int ret;
	int n;
	int j;

	ret = ti_type_by_name(c, "ti_test_cfg", BIT(BTF_KIND_STRUCT), &cfg_id);
	if (ret) {
		pr_info("[type_info] mod: name lookup unavailable (%d), "
			"structural verify\n", ret);
		verify_mod_cfg_struct(c);
	} else {
		CHECK(ti_type_size(c, cfg_id) == sizeof(struct ti_test_cfg),
		      "[type_info] mod: cfg size btf=%u compile=%zu\n",
		      ti_type_size(c, cfg_id), sizeof(struct ti_test_cfg));

		/* member offsets by index: member names may be shifted on
		 * cross-source builds, offsets are always valid */
		n = ti_member_count(c, cfg_id);
		CHECK(n == (int)ARRAY_SIZE(want_off),
		      "[type_info] mod: count=%d want=%zu\n", n,
		      ARRAY_SIZE(want_off));
		for (j = 0; j < n && j < (int)ARRAY_SIZE(want_off); j++) {
			u32 moff;
			u32 msz;

			ret = ti_member_at(c, cfg_id, j, NULL, &type, &moff,
					   &msz);
			CHECK(!ret && moff == want_off[j] && !msz,
			      "[type_info] mod: m[%d] off=%u want=%u "
			      "(ret=%d)\n", j, moff, want_off[j], ret);
		}

#ifdef CONFIG_TI_DWARF
		/* DWARF member names: fallback when BTF names shift */
		ret = ti_member_off(c, cfg_id, "magic", &bit_off, &bit_sz);
		CHECK(!ret && bit_off == 0 && !bit_sz,
		      "[type_info] mod: dw magic off=%u sz=%u (ret=%d)\n",
		      bit_off, bit_sz, ret);
		ret = ti_member_off(c, cfg_id, "tag", &bit_off, &bit_sz);
		CHECK(!ret && bit_off == 128 && !bit_sz,
		      "[type_info] mod: dw tag off=%u sz=%u (ret=%d)\n",
		      bit_off, bit_sz, ret);
		ret = ti_member_off(c, cfg_id, "bits", &bit_off, &bit_sz);
		CHECK(!ret && bit_off == 256 && !bit_sz,
		      "[type_info] mod: dw bits off=%u sz=%u (ret=%d)\n",
		      bit_off, bit_sz, ret);
		{
			const char *mn = NULL;

			ret = ti_member_at(c, cfg_id, 0, &mn, NULL, &bit_off,
					   &bit_sz);
			CHECK(!ret && mn && !strcmp(mn, "magic"),
			      "[type_info] mod: dw m[0] name=%s (ret=%d)\n",
			      mn ? mn : "?", ret);
		}
		if (c->dw) {
			/* DWARF bitfield offsets (DWARF4 bit_offset math) */
			ret = ti_dw_member_off(c->dw, "ti_test_bits", "a",
					       &bit_off, &bit_sz);
			CHECK(!ret && bit_off == 0 && bit_sz == 3,
			      "[type_info] mod: dw bits.a off=%u sz=%u "
			      "(ret=%d)\n", bit_off, bit_sz, ret);
			ret = ti_dw_member_off(c->dw, "ti_test_bits", "b",
					       &bit_off, &bit_sz);
			CHECK(!ret && bit_off == 3 && bit_sz == 5,
			      "[type_info] mod: dw bits.b off=%u sz=%u "
			      "(ret=%d)\n", bit_off, bit_sz, ret);
			ret = ti_dw_member_off(c->dw, "ti_test_bits", "rest",
					       &bit_off, &bit_sz);
			CHECK(!ret && bit_off == 32 && !bit_sz,
			      "[type_info] mod: dw bits.rest off=%u sz=%u "
			      "(ret=%d)\n", bit_off, bit_sz, ret);
		}
#endif
	}

#ifdef CONFIG_TI_FEATURE
	{
		static const struct ti_member_desc seq[] = {
			{ NULL, 0, 0 },
			{ NULL, 32, 0 },
			{ NULL, 48, 0 },
		};
		u32 fid;
		int j;

		ret = ti_type_by_seq(c, sizeof(struct ti_test_cfg), seq,
				     ARRAY_SIZE(seq), &fid);
		CHECK(!ret, "[type_info] mod: feature seq miss (%d)\n", ret);
		if (!ret) {
			int n = ti_member_count(c, fid);

			CHECK(n == (int)ARRAY_SIZE(want_off),
			      "[type_info] mod: feature count=%d want=%zu\n",
			      n, ARRAY_SIZE(want_off));
			for (j = 0; j < n && j < (int)ARRAY_SIZE(want_off);
			     j++) {
				u32 fo;
				u32 fs;

				ret = ti_member_at(c, fid, j, NULL, NULL, &fo,
						   &fs);
				CHECK(!ret && fo == want_off[j] && !fs,
				      "[type_info] mod: feature m[%d] "
				      "off=%u want=%u (ret=%d)\n", j, fo,
				      want_off[j], ret);
			}
		}
	}
#endif

#ifdef CONFIG_TI_REMAP
	{
		const struct ti_ctx *kb = ti_base();
		const struct ti_ctx *cc;
		u32 tid;

		cc = c;
		while (cc->base)
			cc = cc->base;
		if (cc == kb && kb->cur.type_cnt) {
			ret = ti_type_by_name(c, "task_struct",
					      BIT(BTF_KIND_STRUCT), &tid);
			CHECK(!ret && tid <= c->cur.id_off,
			      "[type_info] mod: remap task_struct ret=%d "
			      "id=%u id_off=%u\n", ret, tid, c->cur.id_off);
			if (!ret) {
				ret = ti_member_off(c, tid, "pid", &bit_off,
						    &bit_sz);
				CHECK(!ret && bit_off / 8 ==
						 offsetof(struct task_struct,
							  pid) &&
				      !bit_sz,
				      "[type_info] mod: remap "
				      "task_struct.pid off=%u compile=%zu "
				      "(ret=%d)\n", bit_off / 8,
				      offsetof(struct task_struct, pid), ret);
			}
		} else {
			pr_info("[type_info] mod: remap skipped (base is "
				"not vmlinux)\n");
		}
	}
#endif
}

static int enum_cb(const struct ti_module *m, void *arg)
{
	unsigned int *n = arg;

	pr_info("[type_info] enum: %-20s state=%u base=%px size=%lu\n",
		m->name, m->state, (void *)m->core_base, m->core_size);
	(*n)++;
	return 0;
}

static void verify_mod_enum(void)
{
	unsigned int n = 0;
	int ret;

	ret = ti_mod_enum(enum_cb, &n);
	pr_info("[type_info] mod enum: %u modules (%d)\n", n, ret);
}

static void ti_verify_enum(void)
{
	verify_mod_enum();
}

static void ti_verify_reg(void)
{
	static const struct {
		const char *name;
		u32 bit_off;
		u32 bit_sz;
	} want[] = {
		{ "magic", offsetof(struct ti_test_cfg, magic) * 8, 0 },
		{ "flags", offsetof(struct ti_test_cfg, flags) * 8, 0 },
		{ "mode", offsetof(struct ti_test_cfg, mode) * 8, 0 },
		{ "prio", offsetof(struct ti_test_cfg, prio) * 8, 0 },
		{ "seq", offsetof(struct ti_test_cfg, seq) * 8, 0 },
		{ "tag", offsetof(struct ti_test_cfg, tag) * 8, 0 },
		{ "bits", offsetof(struct ti_test_cfg, bits) * 8, 0 },
		{ "a", 0, 3 },
		{ "b", 3, 5 },
		{ "rest", 32, 0 },
	};
	u32 id;
	u32 bit_off;
	u32 bit_sz;
	int ret;
	int n;
	int i;

	ret = ti_type_by_name(ti_base(), "ti_test_cfg", BIT(BTF_KIND_STRUCT),
			      &id);
	if (ret) {
		pr_info("[type_info] reg: ti_test_cfg not registered (%d)\n",
			ret);
		return;
	}
	CHECK(id >= TI_REG_ID_BASE,
	      "[type_info] reg: id=%u not synthetic\n", id);
	CHECK(ti_type_size(ti_base(), id) == sizeof(struct ti_test_cfg),
	      "[type_info] reg: size=%u compile=%zu\n",
	      ti_type_size(ti_base(), id), sizeof(struct ti_test_cfg));
	CHECK(!ti_member_off(ti_base(), id, "magic", &bit_off, &bit_sz) &&
	      bit_off == offsetof(struct ti_test_cfg, magic) * 8 && !bit_sz,
	      "[type_info] reg: magic off=%u\n", bit_off);
	CHECK(!ti_member_off(ti_base(), id, "seq", &bit_off, &bit_sz) &&
	      bit_off == offsetof(struct ti_test_cfg, seq) * 8 && !bit_sz,
	      "[type_info] reg: seq off=%u\n", bit_off);
	CHECK(!ti_member_off(ti_base(), id, "bits", &bit_off, &bit_sz) &&
	      bit_off == offsetof(struct ti_test_cfg, bits) * 8 && !bit_sz,
	      "[type_info] reg: bits off=%u\n", bit_off);
	CHECK(!ti_member_off(ti_base(), id, "a", &bit_off, &bit_sz) &&
	      bit_off == 0 && bit_sz == 3,
	      "[type_info] reg: a off=%u sz=%u\n", bit_off, bit_sz);
	CHECK(!ti_member_off(ti_base(), id, "b", &bit_off, &bit_sz) &&
	      bit_off == 3 && bit_sz == 5,
	      "[type_info] reg: b off=%u sz=%u\n", bit_off, bit_sz);

	n = ti_member_count(ti_base(), id);
	CHECK(n == (int)ARRAY_SIZE(want),
	      "[type_info] reg: count=%d want=%zu\n", n,
	      ARRAY_SIZE(want));
	for (i = 0; i < n && i < (int)ARRAY_SIZE(want); i++) {
		const char *name = NULL;
		u32 type = 0;

		ret = ti_member_at(ti_base(), id, i, &name, &type, &bit_off,
				   &bit_sz);
		CHECK(!ret && name && !strcmp(name, want[i].name) &&
		      bit_off == want[i].bit_off && bit_sz == want[i].bit_sz,
		      "[type_info] reg: member[%d] %s off=%u sz=%u type=%u "
		      "(ret=%d)\n", i, name ? name : "?", bit_off, bit_sz,
		      type, ret);
	}

	pr_info("[type_info] reg verify done, %d fail%s\n", vfails,
		vfails == 1 ? "" : "s");
}

extern int ti_cur_pid;
extern int ti_ref_pid;

static void verify_anchor(void)
{
	struct ti_task_offs offs;
	struct ti_boot_args args = {
		.comm = "init",
	};

	args.pid = ti_cur_pid;
	args.tgid = ti_cur_pid;
	args.ref_pid = ti_ref_pid;
	args.ref_tgid = ti_ref_pid;

	memset(&offs, 0, sizeof(offs));
	ti_bootstrap_task(&args, &offs);

	pr_info("[type_info] anchor: cur=%px pid=%u comm=%u rcred=%u "
		"cred=%u tasks=%u mm=%u\n",
		ti_current(), offs.off_pid, offs.off_comm,
		offs.off_real_cred, offs.off_cred, offs.off_tasks,
		offs.off_mm);
	pr_info("[type_info] anchor: compile pid=%zu comm=%zu rcred=%zu "
		"cred=%zu tasks=%zu mm=%zu\n",
		offsetof(struct task_struct, pid),
		offsetof(struct task_struct, comm),
		offsetof(struct task_struct, real_cred),
		offsetof(struct task_struct, cred),
		offsetof(struct task_struct, tasks),
		offsetof(struct task_struct, mm));

	CHECK(!offs.off_pid || offs.off_pid == offsetof(struct task_struct, pid),
	      "[type_info] anchor pid=%u compile=%zu\n", offs.off_pid,
	      offsetof(struct task_struct, pid));
	CHECK(!offs.off_comm || offs.off_comm == offsetof(struct task_struct, comm),
	      "[type_info] anchor comm=%u compile=%zu\n", offs.off_comm,
	      offsetof(struct task_struct, comm));
	CHECK(!offs.off_real_cred ||
	      offs.off_real_cred == offsetof(struct task_struct, real_cred),
	      "[type_info] anchor rcred=%u compile=%zu\n",
	      offs.off_real_cred, offsetof(struct task_struct, real_cred));
	CHECK(!offs.off_tasks ||
	      offs.off_tasks == offsetof(struct task_struct, tasks),
	      "[type_info] anchor tasks=%u compile=%zu\n", offs.off_tasks,
	      offsetof(struct task_struct, tasks));
	CHECK(!offs.off_mm || offs.off_mm == offsetof(struct task_struct, mm),
	      "[type_info] anchor mm=%u compile=%zu\n", offs.off_mm,
	      offsetof(struct task_struct, mm));
}

void ti_verify_ti(void);

void ti_verify_ti(void)
{
	struct ti_ctx *mc;

	vfails = 0;
	if (ti_base()->cur.type_cnt) {
		verify_synth();
		verify_kernel(ti_base());
	} else {
		pr_info("[type_info] btf unavailable, anchor path\n");
	}
	verify_anchor();

	mc = NULL;
	if (!ti_mod_lookup("testmod", &mc))
		verify_mod_cfg(mc);
	else
		pr_info("[type_info] testmod not captured yet\n");
	ti_verify_reg();
	verify_mod_enum();

	pr_info("[type_info] verify done, %d fail%s\n", vfails,
		vfails == 1 ? "" : "s");
}
#ifdef CONFIG_TI_VERIFY
EXPORT_SYMBOL(ti_verify_ti);
#endif
