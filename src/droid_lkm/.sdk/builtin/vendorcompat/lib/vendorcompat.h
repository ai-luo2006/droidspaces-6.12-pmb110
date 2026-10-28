// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef VENDORCOMPAT_H
#define VENDORCOMPAT_H

#include <linux/types.h>

struct hk_redir;

/*
 * choosing the route, once, so a consumer does not carry a table of what each
 * kernel allows. the consumer names what it wants replaced, this asks the page
 * table and the breakpoint gates which route stands a chance on this kernel, takes
 * it, and reports which one it took
 *
 * a kernel that does not let a read only table be written shows up as the slot
 * route not being offered, and the function itself still being one: the route that
 * needs a write to data is the one that fails there, and the route that writes the
 * code is the one that works
 */

enum vc_route {
	VC_ROUTE_NONE = 0,	/* nothing stands a chance, nothing was changed */
	VC_ROUTE_TEXT,		/* the entry itself was written */
	VC_ROUTE_KPROBE,	/* the entry keeps its bytes, a breakpoint carries the call */
	VC_ROUTE_SLOT,		/* a pointer in a table was replaced */
};

struct vc_result {
	enum vc_route route;
	bool writable;		/* the page was writable when it was probed */
	bool block_map;		/* no pte to change under it */
	int ret;		/* what the route answered, 0 when it was taken */
};

/*
 * replace the function named sym with repl_sym, on whichever route this kernel
 * allows. the routes are tried in the order that changes least first: the text
 * route only where the page is writable, then the breakpoint route. nothing is
 * written when both refuse, and the result says what was seen
 */
int vc_install(const char *sym, const char *repl_sym, struct vc_result *out);

/* the same by address, which only the breakpoint route takes */
int vc_install_at(unsigned long addr, unsigned long repl, struct vc_result *out);

/* take back everything this library installed. safe when nothing was */
void vc_exit(void);

/* how many replacements are in place */
unsigned int vc_active(void);

/* the name of a route, for a report */
const char *vc_route_name(enum vc_route route);

/*
 * the handle of a replacement that is live, so the replacement can call what it
 * replaced and clear the guard the breakpoint route keeps. null when sym was not
 * replaced here
 */
struct hk_redir *vc_handle(const char *sym);

#endif
