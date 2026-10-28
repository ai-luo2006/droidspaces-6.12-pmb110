// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/compiler_types.h>
#include <linux/printk.h>
#include <linux/export.h>
#include <asm/processor.h>

#include "anchor.h"
#include "port.h"
#include "type_info.h"

#define TI_TASK_WIN	0x2000
#define TI_MM_WIN	0x1000
#define TI_CAND_MAX	16
#define TI_MOD_WIN	0x1000
#define TI_MOD_NODES	64

int ti_scan_bytes(const void *base, u32 range, const void *sample, u32 len,
		  u32 *off)
{
	u32 i;

	if (!len || len > range)
		return -EINVAL;
	for (i = 0; i + len <= range; i++)
		if (!memcmp((const char *)base + i, sample, len)) {
			*off = i;
			return 0;
		}
	return -ENOENT;
}

int ti_scan_u32(const void *base, u32 range, u32 val, u32 *off)
{
	const u32 *p = base;
	u32 n = range / 4;
	u32 i;

	for (i = 0; i < n; i++)
		if (p[i] == val) {
			*off = i * 4;
			return 0;
		}
	return -ENOENT;
}

int ti_scan_ptrpair(const void *base, u32 range, u32 *off)
{
	const unsigned long *p = base;
	u32 n = range / 8;
	u32 i;

	for (i = 0; i + 1 < n; i++)
		if (p[i] && p[i] == p[i + 1]) {
			*off = i * 8;
			return 0;
		}
	return -ENOENT;
}

int ti_scan_ptrpair_rev(const void *base, u32 range, u32 *off)
{
	const unsigned long *p = base;
	u32 n = range / 8;
	u32 i;

	for (i = n; i >= 1; i--)
		if (p[i - 1] && p[i - 1] == p[i]) {
			*off = (i - 1) * 8;
			return 0;
		}
	return -ENOENT;
}
#ifdef CONFIG_TI_PUBLIC_ANCHOR
EXPORT_SYMBOL(ti_scan_bytes);
EXPORT_SYMBOL(ti_scan_u32);
EXPORT_SYMBOL(ti_scan_ptrpair);
EXPORT_SYMBOL(ti_scan_ptrpair_rev);
#endif


static int mm_is_valid(const void *mm)
{
	unsigned long task_size;
	u32 i;

	/* mm_struct contains task_size (TASK_SIZE) near the start */
	for (i = 0; i + 8 <= TI_MM_WIN; i += 8) {
		if (ti_safe_read(&task_size, (const char *)mm + i, 8))
			return -ENOENT;
		if (task_size == TASK_SIZE)
			return i;
	}
	return -ENOENT;
}

static unsigned long (*ti_anchor_resolve)(const char *name);

void ti_anchor_init(unsigned long (*resolve)(const char *name))
{
	ti_anchor_resolve = resolve;
}

static __nocfi unsigned long ti_resolve(const char *name)
{
	return ti_anchor_resolve(name);
}

static __nocfi void *ti_call_find_task(unsigned long fn, pid_t pid)
{
	return ((void *(*)(pid_t))fn)(pid);
}

static u32 task_pidpair_cands(const void *task, pid_t pid, pid_t tgid,
			      u32 *cands, u32 max)
{
	const u32 *p = task;
	u32 n = TI_TASK_WIN / 4;
	u32 cnt = 0;
	u32 i;

	for (i = 0; i + 1 < n && cnt < max; i++)
		if (p[i] == (u32)pid && p[i + 1] == (u32)tgid)
			cands[cnt++] = i * 4;
	return cnt;
}

static u32 pid_cross_verify(const void *cur, const struct ti_boot_args *args)
{
	u32 c1[TI_CAND_MAX];
	u32 c2[TI_CAND_MAX];
	u32 n1;
	u32 n2 = 0;
	u32 best = 0;
	u32 i;
	u32 j;
	unsigned long it = 0;

	n1 = task_pidpair_cands(cur, args->pid, args->tgid, c1, TI_CAND_MAX);
	if (ti_anchor_resolve)
		it = ti_resolve("init_task");

	if (ti_anchor_resolve && args->ref_pid) {
		unsigned long fn = ti_resolve("find_task_by_vpid");

		if (fn) {
			void *ref = ti_call_find_task(fn, args->ref_pid);

			if (ref && ref != cur)
				n2 = task_pidpair_cands(ref, args->ref_pid,
							args->ref_tgid, c2,
							TI_CAND_MAX);
		}
	}
	for (i = 0; i < n1; i++) {
		for (j = 0; j < n2; j++) {
			if (c1[i] != c2[j])
				continue;
			if (best && best != c1[i])
				return 0;
			best = c1[i];
		}
	}
	if (best)
		return best;

	/* init_task filter: true offset must read (0,0) there */
	if (it) {
		for (i = 0; i < n1; i++) {
			u32 a = *(u32 *)((char *)it + c1[i]);
			u32 b = *(u32 *)((char *)it + c1[i] + 4);

			if (a || b)
				continue;
			if (best && best != c1[i])
				return 0;
			best = c1[i];
		}
		if (best)
			return best;
	}
	if (!n2 && !args->ref_pid && !it)
		return n1 ? c1[0] : 0;
	return 0;
}

static int task_tasks_steps(const void *cur, u32 off, u32 off_comm,
			     u32 off_pid)
{
	const void *t = cur;
	u32 steps;
	u32 i;

	for (steps = 0; steps < 256; steps++) {
		unsigned long next = *(unsigned long *)((char *)t + off);
		unsigned long prev = *(unsigned long *)((char *)t + off + 8);
		void *t2;
		unsigned long t2_prev;
		unsigned char comm[16];

		if ((next & 7) || (prev & 7) || !next || !prev)
			return -1;
		t2 = (void *)(next - off);
		if (t2 == cur)
			return steps + 1;
		if (t2 == t)
			return steps + 1;
		if (off_comm) {
			int nonzero = 0;

			if (ti_safe_read(comm, (char *)t2 + off_comm, 16))
				return -1;
			for (i = 0; i < 16; i++)
				if (comm[i])
					nonzero = 1;
			if (!nonzero)
				return -1;
		}
		if (off_pid) {
			u32 pidv;

			if (ti_safe_read(&pidv, (char *)t2 + off_pid, 4))
				return -1;
			if (pidv > 0x100000)
				return -1;
		}
		if (ti_safe_read(&t2_prev, (char *)t2 + off + 8, 8))
			return -1;
		if (t2_prev != (unsigned long)((char *)t + off))
			return -1;
		t = t2;
	}
	return 256;
}

int ti_bootstrap_task(const struct ti_boot_args *args, struct ti_task_offs *out)
{
	void *cur = ti_current();
	unsigned long *p = cur;
	u32 n = TI_TASK_WIN / 8;
	u32 i;
	u32 off;
	int ret;

	memset(out, 0, sizeof(*out));

	ret = ti_scan_bytes(cur, TI_TASK_WIN, args->comm,
			    strnlen(args->comm, 16), &off);
	if (!ret)
		out->off_comm = off;

	if (out->off_comm)
		ret = ti_scan_ptrpair_rev(cur, out->off_comm, &off);
	else
		ret = ti_scan_ptrpair(cur, TI_TASK_WIN, &off);
	if (!ret) {
		out->off_real_cred = off;
		out->off_cred = off + 8;
	}

	out->off_pid = pid_cross_verify(cur, args);

	{
		u32 best_off = 0;
		u32 best_steps = 0;

		for (i = 8; i + 16 <= n * 8; i += 8) {
			unsigned long nx = *(unsigned long *)((char *)cur + i);
			unsigned long pv = *(unsigned long *)((char *)cur + i + 8);
			int st;

			if ((nx & 7) || (pv & 7) || !nx || !pv)
				continue;
			st = task_tasks_steps(cur, i, out->off_comm,
					      out->off_pid);
			if (st > (int)best_steps) {
				best_steps = st;
				best_off = i;
			}
		}
		if (best_steps >= 8)
			out->off_tasks = best_off;
	}

	for (i = 0; i < n; i++) {
		unsigned long cand = p[i];

		if (!cand || (cand & 7))
			continue;
		ret = mm_is_valid((const void *)cand);
		if (ret >= 0) {
			out->off_mm = i * 8;
			out->mm_pgd = ret + 8;
			break;
		}
	}
	return 0;
}

static int mod_chain_nodes(unsigned long head, unsigned long *nodes, u32 max,
			   u32 *cnt)
{
	unsigned long cur;
	unsigned long prev = head;
	unsigned long next;
	unsigned long pv;
	u32 n = 0;

	if (ti_safe_read(&next, (void *)(head + 0), 8))
		return -1;
	if (ti_safe_read(&pv, (void *)(head + 8), 8))
		return -1;
	if ((next & 7) || (pv & 7) || !next || !pv)
		return -1;
	cur = next;
	if (cur == head) {
		*cnt = 0;
		return 0;
	}
	for (;;) {
		if (ti_safe_read(&next, (void *)(cur + 0), 8))
			return -1;
		if (ti_safe_read(&pv, (void *)(cur + 8), 8))
			return -1;
		if ((next & 7) || (pv & 7) || !next || !pv)
			return -1;
		if (pv != prev)
			return -1;
		nodes[n] = cur;
		n++;
		if (n >= max)
			return -1;
		prev = cur;
		cur = next;
		if (cur == head)
			break;
	}
	*cnt = n;
	return 0;
}

static int mod_chain_has(const unsigned long *nodes, u32 n,
			 unsigned long addr)
{
	u32 i;

	for (i = 0; i < n; i++)
		if (nodes[i] == addr)
			return 1;
	return 0;
}

static char ti_mod_name[MODULE_NAME_LEN];

int ti_anchor_set_modname(const char *name)
{
	if (!name || !name[0])
		return -EINVAL;
	strscpy(ti_mod_name, name, sizeof(ti_mod_name));
	return 0;
}
#ifdef CONFIG_TI_MODNAME
EXPORT_SYMBOL(ti_anchor_set_modname);
#endif

static const char *ti_mod_self_name(unsigned long self, char *buf, u32 bufsz)
{
	u32 i;
	char fb[MODULE_NAME_LEN];
	bool have_fb = false;

	if (ti_mod_name[0])
		return ti_mod_name;

	for (i = 0; i + 3 < TI_MOD_WIN; ) {
		u32 len = 0;
		char cand[MODULE_NAME_LEN];

		if (!isalpha(((char *)self)[i]) &&
		    ((char *)self)[i] != '_') {
			i++;
			continue;
		}
		while (i + len < TI_MOD_WIN &&
		       len + 1 < MODULE_NAME_LEN &&
		       ((char *)self)[i + len]) {
			char c = ((char *)self)[i + len];

			if (c < 0x20 || c > 0x7e)
				break;
			len++;
		}
		if (len < 3) {
			i++;
			continue;
		}
		memcpy(cand, (char *)self + i, len);
		cand[len] = 0;
		i += len;

		if (!have_fb) {
			strscpy(fb, cand, sizeof(fb));
			have_fb = true;
		}
		if (ti_anchor_resolve) {
			char sym[64];
			int k;

			for (k = 0; k < 2; k++) {
				snprintf(sym, sizeof(sym), "%s_%s", cand,
					 k ? "exit" : "init");
				if (ti_resolve(sym)) {
					strscpy(buf, cand, bufsz);
					return buf;
				}
			}
		} else {
			strscpy(buf, cand, bufsz);
			return buf;
		}
	}
	/* no candidate passed symbol verification: fall back to the
	 * first string found (static init symbols are stripped by LTO) */
	if (have_fb) {
		strscpy(buf, fb, bufsz);
		return buf;
	}
	return NULL;
}

int ti_bootstrap_module(struct ti_module_offs *out)
{
	unsigned long head;
	unsigned long nodes[TI_MOD_NODES];
	unsigned long self;
	u32 ncnt = 0;
	u32 i;
	u32 j;
	u32 list_off = 0;

	memset(out, 0, sizeof(*out));
	if (!ti_anchor_resolve)
		return -ENOENT;

	head = ti_resolve("modules");
	if (!head)
		return -ENOENT;
	if (mod_chain_nodes(head, nodes, TI_MOD_NODES, &ncnt))
		return -ENOENT;
	if (!ncnt)
		return -ENOENT;

	self = (unsigned long)&__this_module;

	/* name anchor: own module name string, NUL terminated */
	{
		char modname[MODULE_NAME_LEN];
		const char *mn = ti_mod_self_name(self, modname,
						 sizeof(modname));
		u32 mnlen;

		if (!mn)
			return -ENOENT;
		mnlen = strnlen(mn, MODULE_NAME_LEN);
		for (i = 0; i + mnlen + 1 <= TI_MOD_WIN; i += 8) {
			if (memcmp((char *)self + i, mn, mnlen))
				continue;
			if (((char *)self)[i + mnlen])
				continue;
			out->off_name = i;
			break;
		}
	}
	if (!out->off_name)
		return -ENOENT;

	/* list anchor: own list node must be on the chain */
	for (i = 8; i + 16 <= TI_MOD_WIN; i += 8) {
		unsigned long nx = *(unsigned long *)((char *)self + i);
		unsigned long pv = *(unsigned long *)((char *)self + i + 8);

		if ((nx & 7) || (pv & 7) || !nx || !pv)
			continue;
		if (!mod_chain_has(nodes, ncnt, self + i))
			continue;
		if (!mod_chain_has(nodes, ncnt, nx) && nx != head)
			continue;
		if (!mod_chain_has(nodes, ncnt, pv) && pv != head)
			continue;
		list_off = i;
		break;
	}
	if (!list_off)
		return -ENOENT;
	out->off_list = list_off;

	/* state anchor: u32 0..3, consistent across all nodes */
	for (i = 0; i + 4 <= list_off; i += 4) {
		u32 v = *(u32 *)((char *)self + i);

		if (v > 3)
			continue;
		for (j = 0; j < ncnt; j++) {
			u32 sv;

			if (ti_safe_read(&sv,
					 (void *)(nodes[j] - list_off + i), 4))
				break;
			if (sv > 3)
				break;
		}
		if (j == ncnt) {
			out->off_state = i;
			break;
		}
	}

	/* core layout anchor: (canonical ptr, page-aligned size) pair that
	 * contains this module's own code, consistent across all nodes;
	 * first hit = core_layout */
	for (i = 0; i + 12 <= TI_MOD_WIN; i += 8) {
		unsigned long bp = *(unsigned long *)((char *)self + i);
		u32 sz = *(u32 *)((char *)self + i + 8);
		unsigned long self_code = (unsigned long)ti_bootstrap_module;

		if ((bp & 7) || (bp >> 56) != 0xff)
			continue;
		if (sz < 0x1000 || (sz & 0xfff))
			continue;
		if (self_code < bp || self_code >= bp + sz)
			continue;
		for (j = 0; j < ncnt; j++) {
			unsigned long obp;
			u32 osz;

			if (ti_safe_read(&obp,
					 (void *)(nodes[j] - list_off + i), 8))
				break;
			if (ti_safe_read(&osz,
					 (void *)(nodes[j] - list_off + i + 8),
					 4))
				break;
			if ((obp & 7) || (obp >> 56) != 0xff)
				break;
			if (osz < 0x1000 || (osz & 0xfff))
				break;
			if (self_code < obp || self_code >= obp + osz)
				break;
		}
		if (j == ncnt) {
			out->off_core_base = i;
			out->off_core_size = i + 8;
			break;
		}
	}

	pr_info("[type_info] module anchor: list=%u name=%u state=%u "
		"core=%u/%u nodes=%u\n", out->off_list, out->off_name,
		out->off_state, out->off_core_base, out->off_core_size, ncnt);
	return 0;
}
