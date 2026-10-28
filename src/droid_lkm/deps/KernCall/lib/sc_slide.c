// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/printk.h>

#include "sc.h"
#include "sc_slide.h"

#define SC_SLIDE_BUF_WORDS (18 * 1024)
unsigned int sc_slide_buf[SC_SLIDE_BUF_WORDS];

int sc_slide_init(struct sc_slide_win *w, unsigned long pos,
		  unsigned int chunksz, unsigned int margin)
{
	w->chunksz = chunksz;
	w->margin = margin;
	w->addr = pos;

	if (sc_safe_read(sc_slide_buf, (void *)w->addr, chunksz + margin))
		return -1;

	w->off = 0;
	return 0;
}

int sc_slide_advance(struct sc_slide_win *w, unsigned int n)
{
	w->off += n;

	if (w->off >= w->chunksz) {
		unsigned long cursor = w->addr + w->off;
		unsigned long new_addr = (cursor - w->margin) & ~0xFFFULL;

		w->addr = new_addr;
		if (sc_safe_read(sc_slide_buf, (void *)w->addr,
				 w->chunksz + w->margin))
			return -1;
		w->off = cursor - w->addr;
	}
	return 0;
}
