// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "core.h"
#include "ddl_hit.h"
#include "vendor_syms.h"
#include "hk_patch.h"

/* fixes the crash the oplus scheduler module causes on screen off */

struct dlc_ddl_entry {
	char comm[TASK_COMM_LEN];
	u64 hit;
};

static struct dlc_ddl_entry *dlc_ddl_table;
static struct dlc_ddl_entry *dlc_ddl_orig;
static void **dlc_ddl_slot;

static u32 *dlc_guard_insn;
static u32 dlc_guard_orig;

/*
 * The guard is silent by design - a patched cbnz returns without a trace, and
 * a vendor build that does not match the shape above is left alone with a
 * pr_warn that dmesg drops within minutes. Neither outcome was visible to the
 * person holding the phone, so both land here instead: one cat, one answer.
 */
static const char *dlc_guard_state = "not-initialised";
static const char *dlc_guard_detail = "init has not run";
static unsigned long dlc_guard_func;
static u32 dlc_guard_seen[3];

static int dlc_guard_show(struct seq_file *m, void *v)
{
	static const char *const what[3] = {
		"cbnz@0x118", "ldp@0x130", "brk@0x144",
	};
	int i;

	seq_printf(m, "state     %s\n", dlc_guard_state);
	seq_printf(m, "detail    %s\n", dlc_guard_detail);
	if (dlc_guard_func) {
		seq_printf(m, "function  %px\n", (void *)dlc_guard_func);
		for (i = 0; i < ARRAY_SIZE(what); i++)
			seq_printf(m, "  %-12s %08x\n", what[i], dlc_guard_seen[i]);
	}
	return 0;
}

static int dlc_guard_open(struct inode *inode, struct file *file)
{
	return single_open(file, dlc_guard_show, NULL);
}

static const struct proc_ops dlc_guard_pops = {
	.proc_open	= dlc_guard_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static struct proc_dir_entry *dlc_guard_pde;

static bool dlc_ddl_is_pointer(unsigned long val)
{
	return val > 0xffff000000000000UL && (val & 0x7) == 0;
}

/*
 * The histogram is a plain array, nothing can be swapped in for it, and the
 * index comes straight out of the task. The vendor module never checks the top
 * of that range - the compiler's UBSAN bounds check is what catches it, and it
 * catches it with BRK #0x512, which takes the phone down instead of skipping
 * one sample.
 *
 * The check sits three instructions in front of the trap:
 *
 *   ldr    w8, [x19, #0x708]
 *   ubfx   x9, x8, #15, #17        ; w8 >= 32768 ?
 *   cbnz   w9, <trap>              ; 0x35000169
 *   mov    w9, #0x18
 *   umaddl x8, w8, w9, x21         ; x21 + index * 24
 *   ldr    x9, [x8, #0x10]
 *   add    x9, x9, #1
 *   str    x9, [x8, #0x10]
 *   ldp    x20, x19, [sp, #0x20]   ; 0xa9424ff4, epilogue starts here
 *   ...
 *   ret
 *
 * The epilogue sits right after the store, so the normal path and the patched
 * branch reach it with the same sp and the same saved registers. Turning the
 * cbnz into a branch there keeps an out of range index out of the table and
 * leaves the caller's frame intact. All three words are checked first; a
 * vendor build that does not have this shape is left alone.
 */
#define DLC_DDL_GUARD_OFF	0x118	/* cbnz w9, .+0x2c */
#define DLC_DDL_RET_OFF		0x130	/* ldp x20, x19, [sp, #0x20] */
#define DLC_DDL_TRAP_OFF	0x144	/* brk #0x512 */
#define DLC_DDL_CBNZ		0x35000169u
#define DLC_DDL_LDP		0xa9424ff4u
#define DLC_DDL_BRK		0xd42aa240u
#define DLC_DDL_BRANCH		0x14000000u

static int dlc_ddl_patch_word(u32 *addr, u32 insn)
{
	struct hk_write w = { .nofault = true, .verify = true };
	int ret;

	w.slot = HK_SLOT_KERNEL;
	ret = hk_write_run(addr, &insn, sizeof(insn), &w);
	if (ret) {
		w.slot = HK_SLOT_LIBRARY;
		ret = hk_write_run(addr, &insn, sizeof(insn), &w);
	}
	if (ret)
		return ret;

	hk_flush_icache((unsigned long)addr);
	return 0;
}

static int dlc_ddl_guard(unsigned long func)
{
	u32 *guard = (u32 *)(func + DLC_DDL_GUARD_OFF);
	u32 *epilogue = (u32 *)(func + DLC_DDL_RET_OFF);
	u32 *trap = (u32 *)(func + DLC_DDL_TRAP_OFF);
	u32 branch = DLC_DDL_BRANCH |
		     ((DLC_DDL_RET_OFF - DLC_DDL_GUARD_OFF) / 4);
	u32 insn, ldp, brk;
	int ret;

	insn = READ_ONCE(*guard);
	ldp = READ_ONCE(*epilogue);
	brk = READ_ONCE(*trap);

	dlc_guard_func = func;
	dlc_guard_seen[0] = insn;
	dlc_guard_seen[1] = ldp;
	dlc_guard_seen[2] = brk;

	if (insn != DLC_DDL_CBNZ || ldp != DLC_DDL_LDP || brk != DLC_DDL_BRK) {
		dlc_guard_state = "shape-mismatch";
		dlc_guard_detail = "vendor build is not the one this fix knows, an out of range index still traps";
		pr_warn("[droid_lkm_vendor] ddl guard: %px reads %08x/%08x/%08x, not the shape this fix knows, left alone\n",
			(void *)func, insn, ldp, brk);
		return -EINVAL;
	}

	ret = dlc_ddl_patch_word(guard, branch);
	if (ret) {
		dlc_guard_state = "patch-failed";
		dlc_guard_detail = "write refused, an out of range index still traps";
		pr_warn("[droid_lkm_vendor] ddl guard: patch failed (%d), an out of range index will still trap\n",
			ret);
		return ret;
	}

	dlc_guard_insn = guard;
	dlc_guard_orig = insn;
	dlc_guard_state = "patched";
	dlc_guard_detail = "an out of range history index returns instead of trapping";
	pr_info("[droid_lkm_vendor] ddl guard: out of range history index now returns instead of trapping\n");
	return 0;
}

int dlc_ddl_hit_init(void)
{
	unsigned long sym = (unsigned long)dlc_vendor_syms.ddl_sdata;
	unsigned long func = (unsigned long)dlc_vendor_syms.ddl_update;
	size_t count = PID_MAX_LIMIT + 1;
	void *slot;
	void *table;

	if (!dlc_guard_pde)
		dlc_guard_pde = proc_create("ddl_guard", 0444, NULL,
					    &dlc_guard_pops);

	if (!sym || !func) {
		dlc_guard_state = "symbol-missing";
		dlc_guard_detail = !sym ? "ddl_sdata not resolved"
					: "update_ddl_hit_history not resolved";
		pr_warn("[droid_lkm_vendor] ddl: no %s, history left alone\n",
			!sym ? "ddl_sdata" : "update_ddl_hit_history");
		return -ENOENT;
	}

	slot = (void *)sym;
	if (!dlc_ddl_is_pointer((unsigned long)READ_ONCE(*(void **)slot))) {
		pr_warn("[droid_lkm_vendor] ddl history is an array so it cannot be replaced\n");
		return dlc_ddl_guard(func);
	}

	table = kvzalloc(count * sizeof(struct dlc_ddl_entry), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	dlc_ddl_table = table;
	dlc_ddl_slot = slot;
	dlc_ddl_orig = READ_ONCE(*(void **)slot);
	WRITE_ONCE(*(void **)slot, table);
	pr_info("[droid_lkm_vendor] ddl history table replaced with %zu entries\n",
		count);
	return 0;
}

void dlc_ddl_hit_exit(void)
{
	if (dlc_guard_pde) {
		proc_remove(dlc_guard_pde);
		dlc_guard_pde = NULL;
	}

	if (dlc_guard_insn) {
		dlc_ddl_patch_word(dlc_guard_insn, dlc_guard_orig);
		dlc_guard_insn = NULL;
		dlc_guard_state = "restored";
		dlc_guard_detail = "module unloaded, the vendor branch is back";
	}

	if (!dlc_ddl_slot || !dlc_ddl_table)
		return;

	WRITE_ONCE(*dlc_ddl_slot, dlc_ddl_orig);
	kvfree(dlc_ddl_table);
	dlc_ddl_table = NULL;
	dlc_ddl_orig = NULL;
	dlc_ddl_slot = NULL;
}
