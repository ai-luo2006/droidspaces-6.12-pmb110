// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/compiler_types.h>

#include "btf.h"
#include "lib.h"
#include "type_info.h"
#include "anchor.h"
#include "port.h"
#include "reg.h"

int ti_mod_anchor __used = 0;
int ti_btf_mode __used = 0;

struct ti_mod_ent {
	char name[MODULE_NAME_LEN];
	struct ti_ctx *ctx;
	void *blob;
	void *mid_blob;
};

static struct ti_resolver ti_res;
static struct ti_ctx ti_base_ctx;
static bool ti_ready_flag;

static struct mutex ti_mod_lock;
static struct notifier_block ti_mod_nb;
static struct ti_mod_ent *ti_mods;
static u32 ti_mod_cnt;
static u32 ti_mod_cap;

static struct ti_module_offs ti_m_offs;
static bool ti_m_offs_ok;
static unsigned long ti_nb_register;
static unsigned long ti_nb_unregister;
static unsigned long ti_mods_addr;

static __nocfi struct module *ti_call_find_module(unsigned long fn,
						  const char *name)
{
	return ((struct module *(*)(const char *))fn)(name);
}

static bool ti_ker_addr(unsigned long v)
{
	return v >= TASK_SIZE;
}

static __nocfi int ti_call_register_nb(unsigned long fn,
				       struct notifier_block *nb)
{
	if (!ti_ker_addr(fn))
		return -EINVAL;
	return ((int (*)(struct notifier_block *))fn)(nb);
}

static __nocfi int ti_call_unregister_nb(unsigned long fn,
					 struct notifier_block *nb)
{
	if (!ti_ker_addr(fn))
		return -EINVAL;
	return ((int (*)(struct notifier_block *))fn)(nb);
}

struct ti_ctx *ti_base(void)
{
	return &ti_base_ctx;
}

bool ti_ready(void)
{
	return ti_ready_flag;
}

u32 ti_type_count(void)
{
	if (!ti_ready_flag)
		return 0;
	return ti_base_ctx.cur.type_cnt;
}

bool ti_btf_available(void)
{
	return ti_ready_flag && ti_base_ctx.cur.type_cnt != 0;
}

static void *ti_blob_dup(const void *src, u32 size)
{
	void *p;

	p = kvmalloc(size, GFP_KERNEL);
	if (!p)
		return NULL;
	if (ti_safe_read(p, src, size)) {
		kvfree(p);
		return NULL;
	}
	return p;
}

static void ti_mod_ent_free(struct ti_mod_ent *e)
{
	if (!e)
		return;
	if (e->ctx && e->ctx->base && e->ctx->base != &ti_base_ctx) {
		ti_ctx_close((struct ti_ctx *)e->ctx->base);
		e->ctx->base = NULL;
	}
	ti_ctx_close(e->ctx);
	vfree(e->mid_blob);
	vfree(e->blob);
	memset(e, 0, sizeof(*e));
}

static void ti_mod_ent_add(struct ti_mod_ent *e)
{
	struct ti_mod_ent *na;

	mutex_lock(&ti_mod_lock);
	if (ti_mod_cnt == ti_mod_cap) {
		u32 ncap = ti_mod_cap ? ti_mod_cap * 2 : 8;

		na = vmalloc(sizeof(*na) * ncap);
		if (!na)
			goto out_fail;
		if (ti_mods) {
			memcpy(na, ti_mods, sizeof(*na) * ti_mod_cnt);
			vfree(ti_mods);
		}
		ti_mods = na;
		ti_mod_cap = ncap;
	}
	ti_mods[ti_mod_cnt++] = *e;
	mutex_unlock(&ti_mod_lock);
	return;

out_fail:
	mutex_unlock(&ti_mod_lock);
	ti_mod_ent_free(e);
}

static void ti_mod_ent_del(const char *name)
{
	u32 i;

	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++) {
		if (strcmp(ti_mods[i].name, name))
			continue;
		ti_mod_ent_free(&ti_mods[i]);
		if (i + 1 < ti_mod_cnt)
			memmove(&ti_mods[i], &ti_mods[i + 1],
				sizeof(*ti_mods) * (ti_mod_cnt - i - 1));
		ti_mod_cnt--;
		break;
	}
	mutex_unlock(&ti_mod_lock);
}

static int ti_mod_capture(struct module *mod)
{
	unsigned long dptr;
	unsigned int dsize;
	struct ti_ctx *mid = NULL;
	struct ti_ctx *mc;
	struct ti_mod_ent ent;
	void *blob;
	void *mid_blob = NULL;
	int ret;

	if (!ti_m_offs_ok)
		return 0;
	if (!ti_m_offs.off_btf_data || !ti_m_offs.off_btf_data_size)
		return 0;

	dptr = *(unsigned long *)((char *)mod + ti_m_offs.off_btf_data);
	dsize = *(unsigned int *)((char *)mod + ti_m_offs.off_btf_data_size);
	if (!dptr || !dsize) {
		pr_info("[type_info] module %s no btf data\n", mod->name);
		return 0;
	}

	blob = ti_blob_dup((const void *)dptr, dsize);
	if (!blob)
		return 0;

	mc = kzalloc(sizeof(*mc), GFP_KERNEL);
	if (!mc) {
		vfree(blob);
		return 0;
	}
	strscpy(mc->name, mod->name, sizeof(mc->name));

	if (ti_btf_mode != TI_BTF_STANDALONE &&
	    ti_m_offs.off_btf_base_data && ti_m_offs.off_btf_base_data_size) {
		unsigned long bptr;
		unsigned int bsize;

		bptr = *(unsigned long *)((char *)mod +
					  ti_m_offs.off_btf_base_data);
		bsize = *(unsigned int *)((char *)mod +
					  ti_m_offs.off_btf_base_data_size);
		if (bptr && bsize) {
			mid_blob = ti_blob_dup((const void *)bptr, bsize);
			if (mid_blob) {
				mid = kzalloc(sizeof(*mid), GFP_KERNEL);
				if (mid) {
					ret = ti_ctx_init(mid, mid_blob, bsize);
					if (ret) {
						kfree(mid);
						mid = NULL;
						vfree(mid_blob);
						mid_blob = NULL;
					} else {
						strscpy(mid->name, mod->name,
							sizeof(mid->name));
						mc->base = mid;
					}
				} else {
					vfree(mid_blob);
					mid_blob = NULL;
				}
			}
		}
	}
	if (ti_btf_mode == TI_BTF_DISTILLED && !mc->base) {
		pr_warn("[type_info] module %s no distilled base, "
			"capture skipped\n", mod->name);
		ti_ctx_close(mid);
		vfree(mid_blob);
		kfree(mc);
		vfree(blob);
		return 0;
	}
	if (ti_btf_mode != TI_BTF_STANDALONE && !mc->base)
		mc->base = &ti_base_ctx;

	ret = ti_ctx_init(mc, blob, dsize);
	if (ret) {
		pr_warn("[type_info] module %s btf open failed: %d\n",
			mod->name, ret);
		ti_ctx_close(mid);
		vfree(mid_blob);
		kfree(mc);
		vfree(blob);
		return 0;
	}

#ifdef CONFIG_TI_DWARF
	{
		struct ti_dw *dw = kzalloc(sizeof(*dw), GFP_KERNEL);

		if (dw) {
			int dr = ti_dw_capture(dw, (const void *)dptr);

			if (dr) {
				pr_info("[type_info] module %s dwarf parse "
					"failed: %d\n", mod->name, dr);
				ti_dw_free(dw);
				kfree(dw);
			} else {
				mc->dw = dw;
				pr_info("[type_info] module %s dwarf: %u "
					"structs\n", mod->name, dw->cnt);
			}
		}
	}
#endif

	memset(&ent, 0, sizeof(ent));
	strscpy(ent.name, mod->name, sizeof(ent.name));
	ent.ctx = mc;
	ent.blob = blob;
	ent.mid_blob = mid_blob;
	ti_mod_ent_add(&ent);
	pr_info("[type_info] module %s btf captured: %u types\n", mod->name,
		mc->cur.type_cnt);
	return 0;
}

/*
 * the offsets are used on other modules, so they have to describe the module that
 * is running this code first. a field read at the wrong offset is a wild pointer
 * inside the notifier chain, which is what a 6.18 kernel made of the old guess, so
 * the chain is only joined when the layout answers for this module by name
 */
static bool ti_mod_offs_trusted(void)
{
	struct module *self = THIS_MODULE;
	const char *name;
	u32 i;

	if (!ti_m_offs_ok || !self)
		return false;
	/* a field past a page cannot be inside a struct module */
	if (ti_m_offs.off_name < 8 || ti_m_offs.off_name > 0x1000)
		return false;
	if (ti_m_offs.off_list < 8 || ti_m_offs.off_list > 0x1000)
		return false;
	name = (const char *)self + ti_m_offs.off_name;
	for (i = 0; i < MODULE_NAME_LEN; i++) {
		u8 c = name[i];

		if (!c)
			break;
		if (c < 0x20 || c > 0x7e)
			return false;
	}
	if (!i || i >= MODULE_NAME_LEN)
		return false;
	return !strcmp(name, KBUILD_MODNAME);
}

static int ti_mod_notify(struct notifier_block *nb, unsigned long ev,
			 void *data)
{
	struct module *mod = data;

	if (!mod || !ti_m_offs_ok)
		return NOTIFY_DONE;
	if (ev == MODULE_STATE_COMING)
		ti_mod_capture(mod);
	else if (ev == MODULE_STATE_GOING)
		ti_mod_ent_del((const char *)mod + ti_m_offs.off_name);
	return NOTIFY_DONE;
}

static int ti_m_offs_field(u32 mod_id, const char *field, u32 *out)
{
	u32 bit_off;
	u32 bit_sz;

	if (ti_member_off(&ti_base_ctx, mod_id, field, &bit_off, &bit_sz))
		return -ENOENT;
	if (bit_sz)
		return -ENOENT;
	*out = bit_off / 8;
	return 0;
}

static int ti_mod_offs_init(void)
{
	u32 mod_id;
	u32 cl_id;
	u32 bit_off;
	u32 bit_sz;
	u32 cl_off;
	u32 b_off;
	int ret;

	ret = ti_type_by_name(&ti_base_ctx, "module", BIT(BTF_KIND_STRUCT),
			      &mod_id);
	if (ret)
		return ret;

	ret = ti_m_offs_field(mod_id, "btf_data", &ti_m_offs.off_btf_data);
	if (ret)
		return -ENOTSUPP;
	ret = ti_m_offs_field(mod_id, "btf_data_size",
			      &ti_m_offs.off_btf_data_size);
	if (ret)
		return -ENOTSUPP;

	if (!ti_m_offs_field(mod_id, "btf_base_data",
			     &ti_m_offs.off_btf_base_data))
		if (ti_m_offs_field(mod_id, "btf_base_data_size",
				    &ti_m_offs.off_btf_base_data_size))
			ti_m_offs.off_btf_base_data = 0;

	if (ti_m_offs_field(mod_id, "list", &ti_m_offs.off_list))
		return -ENOTSUPP;
	if (ti_m_offs_field(mod_id, "name", &ti_m_offs.off_name))
		return -ENOTSUPP;
	if (ti_m_offs_field(mod_id, "state", &ti_m_offs.off_state))
		return -ENOTSUPP;

	cl_off = 0;
	if (!ti_member_off(&ti_base_ctx, mod_id, "core_layout", &bit_off,
			   &bit_sz) && !bit_sz) {
		cl_off = bit_off / 8;
		if (!ti_type_by_name(&ti_base_ctx, "module_layout",
				     BIT(BTF_KIND_STRUCT), &cl_id)) {
			if (!ti_m_offs_field(cl_id, "base", &b_off))
				ti_m_offs.off_core_base = cl_off + b_off;
			if (!ti_m_offs_field(cl_id, "size", &b_off))
				ti_m_offs.off_core_size = cl_off + b_off;
		}
	}

	pr_info("[type_info] module btf offsets: list=%u name=%u state=%u "
		"core=%u/%u btf=%u/%u base=%u/%u\n", ti_m_offs.off_list,
		ti_m_offs.off_name, ti_m_offs.off_state,
		ti_m_offs.off_core_base, ti_m_offs.off_core_size,
		ti_m_offs.off_btf_data, ti_m_offs.off_btf_data_size,
		ti_m_offs.off_btf_base_data,
		ti_m_offs.off_btf_base_data_size);
	return 0;
}

int ti_init(struct ti_resolver *res)
{
	unsigned long start;
	unsigned long stop;
	u32 size;
	int ret;

	if (!res || !res->name_to_addr)
		return -EINVAL;
	ti_res = *res;
	ti_anchor_init(ti_res.name_to_addr);
	ti_reg_init();

	mutex_init(&ti_mod_lock);
	ti_mod_nb.notifier_call = ti_mod_notify;
	ti_mods_addr = ti_res.name_to_addr("modules");

	start = ti_res.name_to_addr("__start_BTF");
	stop = ti_res.name_to_addr("__stop_BTF");
	if (start && stop && stop > start) {
		size = stop - start;
		ret = ti_ctx_init(&ti_base_ctx, (const void *)start, size);
		if (!ret) {
			strscpy(ti_base_ctx.name, "vmlinux",
				sizeof(ti_base_ctx.name));
			pr_info("[type_info] vmlinux btf ready, %u types\n",
				ti_base_ctx.cur.type_cnt);
		} else {
			pr_warn("[type_info] vmlinux btf parse failed: %d\n",
				ret);
		}
	} else {
		pr_info("[type_info] vmlinux btf unavailable\n");
	}
	ti_ready_flag = true;

#ifdef TI_MOD_ANCHOR
	if (ti_mod_anchor || !ti_btf_available() || ti_mod_offs_init()) {
		memset(&ti_m_offs, 0, sizeof(ti_m_offs));
		if (ti_bootstrap_module(&ti_m_offs))
			pr_warn("[type_info] module anchor bootstrap failed\n");
	}
#endif
	if (ti_m_offs.off_list && ti_m_offs.off_name)
		ti_m_offs_ok = true;

	ti_nb_register = ti_res.name_to_addr("register_module_notifier");
	ti_nb_unregister = ti_res.name_to_addr("unregister_module_notifier");
	if (!ti_mod_offs_trusted()) {
		pr_info("[type_info] module layout untrusted, "
			"module btf disabled\n");
	} else if (ti_nb_register && ti_nb_unregister) {
		if (ti_call_register_nb(ti_nb_register, &ti_mod_nb))
			pr_warn("[type_info] module notifier register failed\n");
	} else {
		pr_info("[type_info] module notifier unavailable, "
			"module btf disabled\n");
	}
	return 0;
}

void ti_exit(void)
{
	u32 i;

	if (!ti_ready_flag)
		return;
	if (ti_nb_unregister)
		ti_call_unregister_nb(ti_nb_unregister, &ti_mod_nb);
	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++)
		ti_mod_ent_free(&ti_mods[i]);
	vfree(ti_mods);
	ti_mods = NULL;
	ti_mod_cnt = 0;
	ti_mod_cap = 0;
	mutex_unlock(&ti_mod_lock);
	vfree(ti_base_ctx.name_map);
	btf_close(&ti_base_ctx.cur);
	memset(&ti_base_ctx, 0, sizeof(ti_base_ctx));
	ti_ready_flag = false;
}

int ti_mod_lookup(const char *name, struct ti_ctx **out)
{
	u32 i;
	int ret = -ENOENT;

	if (!name || !out)
		return -EINVAL;
	*out = NULL;
	mutex_lock(&ti_mod_lock);
	for (i = 0; i < ti_mod_cnt; i++) {
		if (strcmp(ti_mods[i].name, name))
			continue;
		*out = ti_mods[i].ctx;
		ret = 0;
		break;
	}
	mutex_unlock(&ti_mod_lock);
	return ret;
}

int ti_mod_enum(int (*cb)(const struct ti_module *m, void *arg), void *arg)
{
	unsigned long cur;
	u32 i = 0;
	int ret = 0;

	if (!cb || !ti_m_offs_ok || !ti_m_offs.off_list ||
	    !ti_m_offs.off_name)
		return -ENOENT;
	if (!ti_mods_addr)
		return -ENOENT;

	if (ti_safe_read(&cur, (void *)ti_mods_addr, 8))
		return -ENOENT;

	while (cur && cur != ti_mods_addr && i < 512) {
		unsigned long base = cur - ti_m_offs.off_list;
		struct ti_module m;

		memset(&m, 0, sizeof(m));
		m.mod = (void *)base;
		m.name = (const char *)base + ti_m_offs.off_name;
		if (ti_m_offs.off_state) {
			if (ti_safe_read(&m.state, (void *)(base +
					   ti_m_offs.off_state), 4))
				break;
		}
		if (ti_m_offs.off_core_base && ti_m_offs.off_core_size) {
			if (ti_safe_read(&m.core_base,
					 (void *)(base +
						  ti_m_offs.off_core_base), 8))
				break;
			if (ti_safe_read(&m.core_size,
					 (void *)(base +
						  ti_m_offs.off_core_size), 4))
				break;
		}
		ret = cb(&m, arg);
		if (ret)
			break;
		if (ti_safe_read(&cur, (void *)cur, 8))
			break;
		i++;
	}
	return ret;
}
