// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

/*
 * this library does not scan for the kernel slide. the object is kept empty so a
 * project that lists it links and builds, and a project that wants slide based
 * resolution links the symbol library's own slide.o in its place
 */
typedef int ti_slide_placeholder;
