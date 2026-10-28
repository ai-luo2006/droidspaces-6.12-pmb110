// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef DROID_LKM_MISC_XT_REG_H
#define DROID_LKM_MISC_XT_REG_H

#include <linux/types.h>

int droid_lkm_misc_xt_init(void);
void droid_lkm_misc_xt_exit(void);
unsigned int droid_lkm_misc_xt_count(void);

#endif
