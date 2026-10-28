// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_PORT_H
#define TYPEMOD_PORT_H

#include <linux/types.h>

#define TI_ARCH_ARM64	1
#define TI_ARCH_UNKNOWN	0

#if defined(CONFIG_ARM64)
#define TI_ARCH TI_ARCH_ARM64
#else
#define TI_ARCH TI_ARCH_UNKNOWN
#endif

int ti_safe_read(void *dst, const void *src, size_t sz);
void *ti_current(void);

#endif
