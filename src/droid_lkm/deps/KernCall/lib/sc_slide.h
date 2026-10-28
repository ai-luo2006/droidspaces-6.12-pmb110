// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef KERNCALL_SC_SLIDE_H
#define KERNCALL_SC_SLIDE_H

#include <linux/types.h>

extern unsigned int sc_slide_buf[];

struct sc_slide_win {
	unsigned long addr;
	unsigned int  chunksz;
	unsigned int  margin;
	unsigned int  off;
};

int sc_slide_init(struct sc_slide_win *w, unsigned long pos,
		  unsigned int chunksz, unsigned int margin);
int sc_slide_advance(struct sc_slide_win *w, unsigned int n);

static inline void *sc_slide_ptr(const struct sc_slide_win *w,
				 const void *buf)
{
	return (unsigned char *)buf + w->off;
}

static inline unsigned long sc_slide_addr(const struct sc_slide_win *w)
{
	return w->addr + w->off;
}

#endif
