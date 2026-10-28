// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/uaccess.h>

#include "port.h"

int ti_safe_read(void *dst, const void *src, size_t sz)
{
	return copy_from_kernel_nofault(dst, src, sz);
}
#ifdef CONFIG_TI_PUBLIC_ANCHOR
EXPORT_SYMBOL(ti_safe_read);
#endif

void *ti_current(void)
{
#if TI_ARCH == TI_ARCH_ARM64
	register unsigned long cur;

	asm volatile("mrs %0, sp_el0" : "=r"(cur));
	return (void *)cur;
#else
	/* TODO: implement ti_current for other arches */
	return NULL;
#endif
}
