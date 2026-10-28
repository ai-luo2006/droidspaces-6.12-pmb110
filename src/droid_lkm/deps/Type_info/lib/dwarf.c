// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <linux/export.h>

#include "dwarf.h"
#include "port.h"

#define TI_DW_MAX_STRUCT	512
#define TI_DW_MAX_MEMBERS	2048
#define TI_DW_SCAN		(1 << 20)
#define TI_DW_CHUNK		4096

/* DWARF constants */
#define DW_TAG_structure_type	0x13
#define DW_TAG_member		0x0d

#define DW_AT_name		0x03
#define DW_AT_byte_size		0x0b
#define DW_AT_bit_offset	0x0c
#define DW_AT_byte_sz		0x0b
#define DW_AT_bit_sz		0x0d
#define DW_AT_data_member_location 0x38
#define DW_AT_data_bit_offset	0x69
#define DW_AT_data_bit_offset_alt 0x6b
#define DW_AT_str_offsets_base	0x74

/* DW_FORM */
#define DW_FORM_addr		0x01
#define DW_FORM_block2		0x03
#define DW_FORM_block4		0x04
#define DW_FORM_data2		0x05
#define DW_FORM_data4		0x06
#define DW_FORM_data8		0x07
#define DW_FORM_string		0x08
#define DW_FORM_block		0x09
#define DW_FORM_block1		0x0a
#define DW_FORM_data1		0x0b
#define DW_FORM_flag		0x0c
#define DW_FORM_sdata		0x0d
#define DW_FORM_strp		0x0e
#define DW_FORM_udata		0x0f
#define DW_FORM_ref_addr	0x10
#define DW_FORM_ref1		0x11
#define DW_FORM_ref2		0x12
#define DW_FORM_ref4		0x13
#define DW_FORM_ref8		0x14
#define DW_FORM_ref_udata	0x15
#define DW_FORM_indirect	0x16
#define DW_FORM_sec_offset	0x17
#define DW_FORM_exprloc		0x18
#define DW_FORM_flag_present	0x19
#define DW_FORM_strx		0x1a

/* R_AARCH64 reloc types used by ET_REL debug sections */
#define R_AARCH64_ABS64		0x101
#define R_AARCH64_ABS32		0x102

static u16 dw_rd16(const u8 *p)
{
	return p[0] | (p[1] << 8);
}

static u32 dw_rd32(const u8 *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u64 dw_rd64(const u8 *p)
{
	u64 v = 0;
	u32 i;

	for (i = 0; i < 8; i++)
		v |= (u64)p[i] << (i * 8);
	return v;
}

static void dw_wr32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static void dw_wr64(u8 *p, u64 v)
{
	u32 i;

	for (i = 0; i < 8; i++)
		p[i] = v >> (i * 8);
}

static u64 dw_rd_uleb(const u8 **pp, const u8 *end)
{
	u64 v = 0;
	u32 shift = 0;

	while (*pp < end && shift < 64) {
		u8 b = *(*pp)++;

		v |= (u64)(b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
	}
	return v;
}

static int dw_add_member(struct ti_dw_struct *s, const char *name,
			 u32 bit_off, u32 bit_sz)
{
	struct ti_dw_member *m;

	if (s->n >= s->cap) {
		u32 ncap = s->cap ? s->cap * 2 : 8;
		struct ti_dw_member *nm;

		if (ncap > TI_DW_MAX_MEMBERS)
			return -ENOSPC;
		nm = krealloc(s->m, ncap * sizeof(*nm), GFP_KERNEL);
		if (!nm)
			return -ENOMEM;
		s->m = nm;
		s->cap = ncap;
	}
	m = &s->m[s->n];
	m->name = kstrdup(name, GFP_KERNEL);
	if (!m->name)
		return -ENOMEM;
	m->bit_off = bit_off;
	m->bit_sz = bit_sz;
	s->n++;
	return 0;
}

static struct ti_dw_struct *dw_add_struct(struct ti_dw *dw, const char *name)
{
	struct ti_dw_struct *s;

	if (dw->cnt >= TI_DW_MAX_STRUCT)
		return NULL;
	s = &dw->st[dw->cnt];
	s->name = kstrdup(name, GFP_KERNEL);
	if (!s->name)
		return NULL;
	s->m = kcalloc(8, sizeof(*s->m), GFP_KERNEL);
	if (!s->m) {
		kfree(s->name);
		s->name = NULL;
		return NULL;
	}
	s->cap = 8;
	dw->cnt++;
	return s;
}

static const char *dw_str_at(const struct ti_dw *dw, u32 off)
{
	if (off >= dw->str_sz)
		return "";
	return (const char *)dw->str + off;
}

struct dw_die_ctx {
	const u8 *abbrev;
	const u8 *abbrev_end;
	u32 str_off_base;
};

struct dw_die_out {
	u64 tag;
	u8 has_children;
	const char *name;
	u64 dml;
	u8 dml_set;
	u64 dbo;
	u8 dbo_set;
	u64 bs;
	u8 bs_set;
	u64 byte_sz;
	u8 byte_sz_set;
	u64 bit_off;
	u8 bit_off_set;
	u64 str_off_base;
	u8 str_off_base_set;
};

static const u8 *dw_abbrev_find(const struct dw_die_ctx *dc, u64 code,
				u64 *tag_out, u8 *hc_out,
				u32 attrs[][2], u32 *nattrs_out)
{
	const u8 *ap = dc->abbrev;

	while (ap < dc->abbrev_end) {
		u64 c = dw_rd_uleb(&ap, dc->abbrev_end);
		u64 tag;
		u32 nattrs = 0;

		if (!c)
			break;
		tag = dw_rd_uleb(&ap, dc->abbrev_end);
		if (ap >= dc->abbrev_end)
			break;
		*hc_out = *ap++;
		for (;;) {
			u64 an, af;

			if (ap >= dc->abbrev_end)
				return NULL;
			an = dw_rd_uleb(&ap, dc->abbrev_end);
			af = dw_rd_uleb(&ap, dc->abbrev_end);
			if (!an && !af)
				break;
			if (nattrs < 32) {
				attrs[nattrs][0] = (u32)an;
				attrs[nattrs][1] = (u32)af;
				nattrs++;
			}
		}
		if (c == code) {
			*tag_out = tag;
			*nattrs_out = nattrs;
			return ap;
		}
	}
	return NULL;
}

static int dw_die_read(const struct dw_die_ctx *dc, const u8 **pp,
		       const u8 *end, const struct ti_dw *dw,
		       struct dw_die_out *o)
{
	const u8 *p = *pp;
	u64 code;
	u32 attrs[32][2];
	u32 nattrs;
	u32 i;

	memset(o, 0, sizeof(*o));
	if (p >= end)
		return 0;
	code = dw_rd_uleb(&p, end);
	if (!code)
		return 0;
	if (!dw_abbrev_find(dc, code, &o->tag, &o->has_children, attrs,
			    &nattrs))
		return -EINVAL;

	for (i = 0; i < nattrs; i++) {
		u32 an = attrs[i][0];
		u32 af = attrs[i][1];
		u64 v = 0;
		const char *s = NULL;

		switch (af) {
		case DW_FORM_addr:
			if (p + 8 > end)
				return -EINVAL;
			p += 8;
			break;
		case DW_FORM_data1:
			if (p >= end)
				return -EINVAL;
			v = *p++;
			break;
		case DW_FORM_data2:
			if (p + 2 > end)
				return -EINVAL;
			v = dw_rd16(p);
			p += 2;
			break;
		case DW_FORM_data4:
			if (p + 4 > end)
				return -EINVAL;
			v = dw_rd32(p);
			p += 4;
			break;
		case DW_FORM_data8:
			if (p + 8 > end)
				return -EINVAL;
			v = dw_rd64(p);
			p += 8;
			break;
		case DW_FORM_flag:
			if (p >= end)
				return -EINVAL;
			v = *p++;
			break;
		case DW_FORM_flag_present:
			v = 1;
			break;
		case DW_FORM_udata:
			v = dw_rd_uleb(&p, end);
			break;
		case DW_FORM_sdata:
			v = (u64)dw_rd_uleb(&p, end);
			break;
		case DW_FORM_string:
			s = (const char *)p;
			while (p < end && *p)
				p++;
			if (p < end)
				p++;
			break;
		case DW_FORM_strp:
			if (p + 4 > end)
				return -EINVAL;
			v = dw_rd32(p);
			p += 4;
			s = dw_str_at(dw, (u32)v);
			break;
		case DW_FORM_strx: {
			u64 idx = dw_rd_uleb(&p, end);
			u32 base = dc->str_off_base;

			if (dw->stroff && idx * 4 + 4 <= dw->stroff_sz &&
			    base <= dw->stroff_sz - 4) {
				u32 off = dw_rd32((const u8 *)dw->stroff +
						  base + idx * 4);

				s = dw_str_at(dw, off);
			}
			break;
		}
		case DW_FORM_block1:
			if (p >= end)
				return -EINVAL;
			if (p + 1 + *p > end)
				return -EINVAL;
			p += 1 + *p;
			break;
		case DW_FORM_block2: {
			u32 len = dw_rd16(p);

			if (p + 2 > end)
				return -EINVAL;
			if (p + 2 + len > end)
				return -EINVAL;
			p += 2 + len;
			break;
		}
		case DW_FORM_block4: {
			u32 len = dw_rd32(p);

			if (p + 4 > end)
				return -EINVAL;
			if (p + 4 + len > end)
				return -EINVAL;
			p += 4 + len;
			break;
		}
		case DW_FORM_block: {
			u64 len = dw_rd_uleb(&p, end);

			if (p + len > end)
				return -EINVAL;
			p += len;
			break;
		}
		case DW_FORM_exprloc: {
			u64 len = dw_rd_uleb(&p, end);
			const u8 *e = p;

			if (p + len > end)
				return -EINVAL;
			while (e < p + len) {
				u8 op = *e++;

				if (op == 0x23 || op == 0x11)	/* plus_uconst, constu */
					v = dw_rd_uleb(&e, p + len);
				else if (op == 0x03)		/* addr */
					e += 8;
				else if (op >= 0x30 && op <= 0x4f)
					;			/* lit0..31 */
				else
					break;
			}
			p += len;
			break;
		}
		case DW_FORM_ref1:
			if (p + 1 > end)
				return -EINVAL;
			p += 1;
			break;
		case DW_FORM_ref2:
			if (p + 2 > end)
				return -EINVAL;
			p += 2;
			break;
		case DW_FORM_ref4:
			if (p + 4 > end)
				return -EINVAL;
			p += 4;
			break;
		case DW_FORM_ref8:
			if (p + 8 > end)
				return -EINVAL;
			p += 8;
			break;
		case DW_FORM_ref_udata:
			(void)dw_rd_uleb(&p, end);
			break;
		case DW_FORM_sec_offset:
			if (p + 4 > end)
				return -EINVAL;
			p += 4;
			break;
		default:
			return -EINVAL;
		}

		if (an == DW_AT_name)
			o->name = s;
		else if (an == DW_AT_data_member_location) {
			o->dml = v;
			o->dml_set = 1;
		} else if (an == DW_AT_data_bit_offset ||
			   an == DW_AT_data_bit_offset_alt) {
			o->dbo = v;
			o->dbo_set = 1;
		} else if (an == DW_AT_bit_sz) {
			o->bs = v;
			o->bs_set = 1;
		} else if (an == DW_AT_byte_sz) {
			o->byte_sz = v;
			o->byte_sz_set = 1;
		} else if (an == DW_AT_bit_offset) {
			o->bit_off = v;
			o->bit_off_set = 1;
		} else if (an == DW_AT_str_offsets_base) {
			o->str_off_base = v;
			o->str_off_base_set = 1;
		}
	}

	*pp = p;
	return 1;
}

static int dw_member_fill(const struct dw_die_out *o, u32 *bit_off,
			  u32 *bit_sz)
{
	u64 bo = 0;
	u32 bs = 0;

	if (o->dbo_set)
		bo = o->dml * 8 + o->dbo;
	else if (o->bit_off_set && o->byte_sz_set)
		bo = o->byte_sz * 8 - o->bit_off - o->bs;
	else if (o->dml_set)
		bo = o->dml * 8;
	if (o->bs_set)
		bs = (u32)o->bs;
	if (bo > 0xffffffffULL)
		return -EINVAL;
	*bit_off = (u32)bo;
	*bit_sz = bs;
	return 0;
}
static int dw_walk(struct ti_dw *dw, const struct dw_die_ctx *dc_in,
		   const u8 **pp, const u8 *end, struct ti_dw_struct *parent,
		   int depth)
{
	struct dw_die_ctx dc = *dc_in;
	const u8 *p = *pp;

	for (;;) {
		struct dw_die_out o;
		struct ti_dw_struct *child = NULL;
		int ret;

		ret = dw_die_read(&dc, &p, end, dw, &o);
		if (ret < 0)
			return ret;
		if (ret == 0) {
			/* null DIE: marks end of children, skip it */
			if (p < end)
				p++;
			*pp = p;
			return 0;
		}
		if (o.str_off_base_set)
			dc.str_off_base = (u32)o.str_off_base;
		if (o.tag == DW_TAG_structure_type) {
			if (o.name)
				child = dw_add_struct(dw, o.name);
		} else if (o.tag == DW_TAG_member && parent && o.name) {
			u32 bit_off;
			u32 bit_sz;

			if (!dw_member_fill(&o, &bit_off, &bit_sz))
				dw_add_member(parent, o.name, bit_off, bit_sz);
		}
		if (o.has_children) {
			if (depth > 16)
				return -ELOOP;
			ret = dw_walk(dw, &dc, &p, end, child, depth + 1);
			if (ret) {
				*pp = p;
				return ret;
			}
		}
	}
	*pp = p;
	return 0;
}

static int dw_find_elf(const u8 *btf, u8 *buf, const u8 **elf_out)
{
	unsigned long pos = (unsigned long)btf;
	u32 scanned = 0;
	u32 i;

	/* backward scan with page aligned windows. the module mirror base
	 * is page aligned (vmalloc), so a window either lies fully inside
	 * the mirror (readable, contains data) or fully before it (hole,
	 * unreadable). a window that covers the ELF header at base is
	 * always readable. */
	while (scanned < TI_DW_SCAN) {
		unsigned long start = (pos - TI_DW_CHUNK) &
				      ~(unsigned long)(TI_DW_CHUNK - 1);

		if (ti_safe_read(buf, (void *)start, TI_DW_CHUNK)) {
			pos = start;
			scanned += TI_DW_CHUNK;
			continue;
		}
		for (i = TI_DW_CHUNK; i >= 4; i--) {
			if (buf[i - 4] == 0x7f && buf[i - 3] == 'E' &&
			    buf[i - 2] == 'L' && buf[i - 1] == 'F') {
				*elf_out = (const u8 *)start + i - 4;
				return 0;
			}
		}
		pos = start;
		scanned += TI_DW_CHUNK;
	}
	return -ENOENT;
}

static int dw_sec_find(const void *btf_data, const char *want,
		       const void **data, u32 *size, u8 *win)
{
	const u8 *elf = NULL;
	const u8 *btf = btf_data;
	u8 *shstr;
	u8 buf[0x40];
	u16 shnum;
	u16 shstrndx;
	u64 shoff;
	u16 shentsz;
	const u8 *base;
	u32 shstr_off;
	u32 shstr_size;
	u32 j;
	int ret = -ENOENT;

	if (dw_find_elf(btf, win, &elf))
		return -ENOENT;
	base = elf;

	if (ti_safe_read(buf, elf, 0x40))
		return -ENOENT;
	shoff = dw_rd64(buf + 0x28);
	shentsz = dw_rd16(buf + 0x3a);
	shnum = dw_rd16(buf + 0x3c);
	shstrndx = dw_rd16(buf + 0x3e);
	if (!shoff || !shnum || shentsz < 0x40 || shstrndx >= shnum)
		return -ENOENT;
	if ((u64)shoff + (u64)shentsz * shnum > TI_DW_SCAN * 2)
		return -ENOENT;

	if (ti_safe_read(buf, (const u8 *)base + shoff + shstrndx * shentsz,
			 0x40))
		return -ENOENT;
	shstr_off = dw_rd64(buf + 0x18);
	shstr_size = dw_rd64(buf + 0x20);
	if (!shstr_size || shstr_size > 0x40000)
		return -ENOENT;

	shstr = kzalloc(shstr_size, GFP_KERNEL);
	if (!shstr)
		return -ENOMEM;
	if (ti_safe_read(shstr, (const u8 *)base + shstr_off, shstr_size))
		goto out;

	for (j = 0; j < shnum; j++) {
		u32 name_off;
		const char *name;
		u64 sh_addr;
		u64 sh_off;
		u64 sh_size;

		if (ti_safe_read(buf, (const u8 *)base + shoff + j * shentsz,
				 0x40))
			goto out;
		name_off = dw_rd32(buf);
		sh_addr = dw_rd64(buf + 0x10);
		sh_off = dw_rd64(buf + 0x18);
		sh_size = dw_rd64(buf + 0x20);
		if (name_off >= shstr_size || sh_size > 0x1000000)
			continue;
		name = (const char *)shstr + name_off;

		if (!strcmp(name, ".BTF")) {
			if (sh_addr != (unsigned long)btf)
				goto out;
			continue;
		}
		if (!strcmp(name, want)) {
			*data = (const void *)(base + sh_off);
			*size = sh_size;
			ret = 0;
			break;
		}
	}
out:
	kfree(shstr);
	return ret;
}

static void *dw_dup(const void *src, u32 size)
{
	void *p = kvmalloc(size, GFP_KERNEL);

	if (p && ti_safe_read(p, src, size)) {
		kvfree(p);
		return NULL;
	}
	return p;
}

static void dw_rela_apply(const void *rela, u32 rela_sz, u8 *sec, u32 sec_sz)
{
	u8 *rbuf;
	u32 n;
	u32 i;

	rbuf = kmalloc(rela_sz, GFP_KERNEL);
	if (!rbuf)
		return;
	if (ti_safe_read(rbuf, rela, rela_sz))
		goto out;
	n = rela_sz / 24;
	for (i = 0; i < n; i++) {
		u64 off = dw_rd64(rbuf + (u64)i * 24);
		u64 info = dw_rd64(rbuf + (u64)i * 24 + 8);
		u64 add = dw_rd64(rbuf + (u64)i * 24 + 16);
		u32 type = info & 0xffffffff;

		if (type == R_AARCH64_ABS32 && off + 4 <= sec_sz)
			dw_wr32(sec + off, (u32)add);
		else if (type == R_AARCH64_ABS64 && off + 8 <= sec_sz)
			dw_wr64(sec + off, add);
	}
out:
	kfree(rbuf);
}

int ti_dw_capture(struct ti_dw *dw, const void *btf_data)
{
	const void *info = NULL;
	const void *abbrev = NULL;
	const void *str = NULL;
	const void *stroff = NULL;
	const void *rela_info = NULL;
	const void *rela_stroff = NULL;
	u32 info_sz = 0;
	u32 abbrev_sz = 0;
	u32 str_sz = 0;
	u32 stroff_sz = 0;
	u32 rela_info_sz = 0;
	u32 rela_stroff_sz = 0;
	const u8 *p;
	const u8 *end;
	u8 *win = NULL;
	int ret;

	win = kmalloc(TI_DW_CHUNK, GFP_KERNEL);
	if (!win)
		return -ENOMEM;
	if (dw_sec_find(btf_data, ".debug_info", &info, &info_sz, win))
		goto out_err;
	if (dw_sec_find(btf_data, ".debug_abbrev", &abbrev, &abbrev_sz, win))
		goto out_err;
	if (dw_sec_find(btf_data, ".debug_str", &str, &str_sz, win))
		goto out_err;
	dw_sec_find(btf_data, ".debug_str_offsets", &stroff, &stroff_sz, win);
	dw_sec_find(btf_data, ".rela.debug_info", &rela_info, &rela_info_sz,
		    win);
	dw_sec_find(btf_data, ".rela.debug_str_offsets", &rela_stroff,
		    &rela_stroff_sz, win);

	dw->info = dw_dup(info, info_sz);
	dw->abbrev = dw_dup(abbrev, abbrev_sz);
	dw->str = dw_dup(str, str_sz);
	if (!dw->info || !dw->abbrev || !dw->str)
		goto out_nomem;
	dw->info_sz = info_sz;
	dw->abbrev_sz = abbrev_sz;
	dw->str_sz = str_sz;

	/* ET_REL debug sections carry placeholder values where the linker
	 * would write string offsets. apply the relocations: the addend is
	 * the offset inside the referenced section. */
	if (rela_info && rela_info_sz)
		dw_rela_apply(rela_info, rela_info_sz, dw->info, dw->info_sz);
	if (stroff && stroff_sz) {
		dw->stroff = dw_dup(stroff, stroff_sz);
		if (dw->stroff) {
			dw->stroff_sz = stroff_sz;
			if (rela_stroff && rela_stroff_sz)
				dw_rela_apply(rela_stroff, rela_stroff_sz,
					      dw->stroff, dw->stroff_sz);
		}
	}

	dw->st = kcalloc(TI_DW_MAX_STRUCT, sizeof(*dw->st), GFP_KERNEL);
	if (!dw->st)
		goto out_nomem;
	dw->cap = TI_DW_MAX_STRUCT;

	p = dw->info;
	end = p + dw->info_sz;
	while (p + 4 <= end) {
		u32 unit_len = dw_rd32(p);
		const u8 *cu_end;
		u16 version;
		u32 ab_off = 0;

		p += 4;
		if (unit_len == 0xffffffff)
			break;		/* DWARF64 unsupported */
		if (!unit_len || (u32)(end - p) < unit_len)
			break;
		cu_end = p + unit_len;
		if (p + 2 > cu_end)
			break;
		version = dw_rd16(p);
		p += 2;
		if (version == 5) {
			if (p + 6 > cu_end)
				break;
			p += 2;		/* unit_type + address_size */
			ab_off = dw_rd32(p);
			p += 4;		/* abbrev_offset */
		} else {
			if (p + 5 > cu_end)
				break;
			ab_off = dw_rd32(p);
			p += 5;		/* abbrev_offset + address_size */
		}
		if (ab_off >= dw->abbrev_sz)
			break;
		{
			struct dw_die_ctx dc = {
				.abbrev = dw->abbrev + ab_off,
				.abbrev_end = dw->abbrev + dw->abbrev_sz,
			};

			ret = dw_walk(dw, &dc, &p, cu_end, NULL, 0);
			if (ret)
				return ret;
		}
		p = cu_end;
	}

	if (!dw->cnt)
		return -ENODATA;
	return 0;

out_err:
	kfree(win);
	return -ENOENT;
out_nomem:
	kfree(win);
	ti_dw_free(dw);
	return -ENOMEM;
}

void ti_dw_free(struct ti_dw *dw)
{
	u32 i;

	if (!dw)
		return;
	for (i = 0; i < dw->cnt; i++) {
		u32 j;

		kfree(dw->st[i].name);
		for (j = 0; j < dw->st[i].n; j++)
			kfree(dw->st[i].m[j].name);
		kfree(dw->st[i].m);
	}
	kfree(dw->st);
	kvfree(dw->stroff);
	kvfree(dw->str);
	kvfree(dw->abbrev);
	kvfree(dw->info);
	memset(dw, 0, sizeof(*dw));
}

static struct ti_dw_struct *dw_find(const struct ti_dw *dw, const char *sname)
{
	u32 i;

	for (i = 0; i < dw->cnt; i++) {
		if (dw->st[i].name && !strcmp(dw->st[i].name, sname))
			return &dw->st[i];
	}
	return NULL;
}

int ti_dw_member_off(const struct ti_dw *dw, const char *sname,
		     const char *member, u32 *bit_off, u32 *bit_sz)
{
	struct ti_dw_struct *s;
	u32 i;

	if (!dw || !sname || !member || !bit_off || !bit_sz)
		return -EINVAL;
	s = dw_find(dw, sname);
	if (!s)
		return -ENOENT;
	for (i = 0; i < s->n; i++) {
		if (s->m[i].name && !strcmp(s->m[i].name, member)) {
			*bit_off = s->m[i].bit_off;
			*bit_sz = s->m[i].bit_sz;
			return 0;
		}
	}
	return -ENOENT;
}

const char *ti_dw_member_name(const struct ti_dw *dw, const char *sname,
			      u32 bit_off, u32 bit_sz)
{
	struct ti_dw_struct *s;
	u32 i;

	if (!dw || !sname)
		return NULL;
	s = dw_find(dw, sname);
	if (!s)
		return NULL;
	for (i = 0; i < s->n; i++) {
		if (s->m[i].bit_off == bit_off && s->m[i].bit_sz == bit_sz)
			return s->m[i].name;
	}
	return NULL;
}

#ifdef CONFIG_TI_DWARF_EXPORT
EXPORT_SYMBOL(ti_dw_capture);
EXPORT_SYMBOL(ti_dw_member_off);
EXPORT_SYMBOL(ti_dw_member_name);
#endif
