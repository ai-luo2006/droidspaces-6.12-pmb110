// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef TYPEMOD_VERIFY_H
#define TYPEMOD_VERIFY_H

#include <linux/types.h>

struct ti_test_bits {
	u32 a : 3;
	u32 b : 5;
	u32 rest;
};

struct ti_test_cfg {
	u32 magic;
	u16 flags;
	u8 mode;
	u8 prio;
	u64 seq;
	char tag[16];
	struct {
		u32 a : 3;
		u32 b : 5;
		u32 rest;
	} bits;
};

#endif
