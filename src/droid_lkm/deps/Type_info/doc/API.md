# type_info API

call ti_init once with a resolver that resolves kernel symbol names.
ti_init parses vmlinux BTF if present, finds the struct module offsets
(BTF or anchor), registers the module notifier for automatic module BTF
capture, and inits the registry. after ti_init use the query functions.

## Dependencies (KallRecon)

symbol resolution comes from KallRecon (see kallrecon/README.md and
kallrecon/doc/API.md). call find_kallsyms_base() first, then pass
kallrecon_klp (the kernel's own kallsyms_lookup_name) or
kallsyms_name_to_addr as ti_resolver.name_to_addr.

kallrecon/lib/core.o and kallrecon/lib/anchor.o are linked into the
module. only sprint_symbol is required from the kernel.

lib is self contained and does not reference src. linking lib/*.o
plus kallrecon suffices for a standalone LKM. src/main.c adds the
module params and the bootstrap entry, src/verify.c adds the self
test (ti_verify_ti), both optional for embedded use.

symbols resolved through the resolver:

- __start_BTF / __stop_BTF: vmlinux BTF blob bounds
- modules: module list head
- register_module_notifier / unregister_module_notifier: module
  notifier registration

any resolver implementation can be swapped in.

## Lifecycle

**int ti_init(struct ti_resolver *res)**

the one call that starts everything. 0 on success. -ENOTSUPP when
vmlinux BTF is unavailable; the anchor path is still used and the
library keeps working.

**void ti_exit(void)**

unregisters the notifier, frees captured module contexts and the
registry.

## Self test

**void ti_verify_ti(void)**

the self test has no export by default. the library triggers it itself, so a build
that only links the library in keeps the whole surface internal; a build that wants
to trigger it from outside adds `TI_VERIFY=1`.

run the full self test: synthetic BTF, kernel layout against
compile-time offsets, anchor offsets, module config, registry and
module enum. prints fail counts, callers check the log. exported,
any LKM can trigger it. this is an example verification, an embedded
lib user can write its own checks against the query API instead.

## Layout source

**struct ti_ctx *ti_base(void)**

the vmlinux context. query kernel structs against it.

**bool ti_btf_available(void)**

nonzero when vmlinux BTF was parsed. zero on no-BTF kernels.

**u32 ti_type_count(void)**

number of vmlinux BTF types. zero when BTF is unavailable.

**int ti_ctx_open(const void *blob, u32 size, struct ti_ctx **out)**

open any BTF blob (for example a module's distilled base) as its own
context. the caller owns the blob memory for the life of the context.

**void ti_ctx_close(struct ti_ctx *ctx)**

free a context created by ti_ctx_open. never pass ti_base() here.

## Query

**int ti_type_by_name(const struct ti_ctx *ctx, const char *name,
u32 kind_mask, u32 *out)**

find a type id by name. kind_mask filters by BTF kind
(BIT(BTF_KIND_STRUCT) for structs); 0 means no filter. hits the
registry when BTF misses and the name was registered. 0 on success,
-ENOENT when not found.

**u32 ti_type_size(const struct ti_ctx *ctx, u32 id)**

type size in bytes. pointers are 8, arrays are element * nelems.
0 when unknown.

**int ti_follow(const struct ti_ctx *ctx, u32 id, u32 *out)**

walk the typedef / const / volatile / restrict / type_tag chain and
return the underlying type id.

**int ti_member_off(const struct ti_ctx *ctx, u32 id,
const char *member, u32 *bit_off, u32 *bit_sz)**

find a member's bit offset. bit_sz is nonzero only for bitfields
(kflag layouts). byte offset = bit_off / 8.

**int ti_member_info(const struct ti_ctx *ctx, u32 id,
const char *member, u32 *type, u32 *bit_off, u32 *bit_sz)**

same as ti_member_off, also returns the member type id.

**int ti_member_count(const struct ti_ctx *ctx, u32 id)**

number of members. works on registry ids too.

**int ti_member_at(const struct ti_ctx *ctx, u32 id, u32 idx,
const char **name, u32 *type, u32 *bit_off, u32 *bit_sz)**

read the idx-th member: name, type, bit offset and bit size. -ENOENT
past the end. with ti_type_by_name + ti_member_count this exports a
full struct layout (member names and offsets).

synthetic ids >= 0x80000000 (TI_REG_ID_BASE) resolve to the registry;
all query functions handle them.

## Function and enum query (CONFIG_TI_FUNC)

kernel functions and enums are BTF types like any other. find a
function by name with ti_type_by_name and kind_mask
BIT(BTF_KIND_FUNC), then read its signature. kernel functions live
in the vmlinux context, their names never shift, query them against
ti_base(). enums work for BTF_KIND_ENUM and BTF_KIND_ENUM64.

**int ti_func_proto(const struct ti_ctx *ctx, u32 func_id,
u32 *ret_type, u32 *nparams)**

read the return type id and the parameter count of a function.
func_id may be a FUNC or FUNC_PROTO id, FUNC is followed
automatically. the return type id resolves like any type id.

**int ti_func_param(const struct ti_ctx *ctx, u32 func_id, u32 idx,
const char **name, u32 *type)**

read the idx-th parameter type and name. -ENOENT past the end.
anonymous parameters have an empty name. pass NULL for name or type
to skip it, both NULL is an existence check.

**int ti_enum_at(const struct ti_ctx *ctx, u32 id, u32 idx,
const char **name, s64 *val)**

read the idx-th enum member. values are s64 for both ENUM and
ENUM64. -ENOENT past the end.

**int ti_enum_val(const struct ti_ctx *ctx, u32 id, const char *name,
s64 *val)**

find an enum member value by name.

## Raw type access (CONFIG_TI_FUNC)

**struct ti_type_raw { u32 kind; u32 vlen; u32 type; const char *name;
const void *data; u32 data_len; }**

one entry for any BTF kind. name is resolved through the string
base, shifted names are handled. data points at the kind specific
payload right after the type header (btf_member / btf_param /
btf_enum64 / btf_array and so on), data_len is the remaining size so
the caller can bound its parse.

**int ti_type_raw(const struct ti_ctx *ctx, u32 id,
struct ti_type_raw *out)**

read any type as raw fields. parse data with the kernel UAPI
structures from include/uapi/linux/btf.h. this is the escape hatch
for kinds without a wrapper above.

## Feature query (CONFIG_TI_FEATURE)

find a struct by its shape instead of its name. works when the name
is unknown or corrupted.

**int ti_type_by_size(const struct ti_ctx *ctx, u32 size, u32 vlen,
u32 *out)**

find a struct or union by size. vlen is the member count, pass 0 to
skip that check. searches own types first, then the base context.

**int ti_type_by_seq(const struct ti_ctx *ctx, u32 size,
const struct ti_member_desc *seq, u32 n, u32 *out)**

find a struct or union whose first n members match the given offsets.
a member name in seq is checked when set, NULL skips the name check.
use this when you know the layout but not the name.

## Remap (CONFIG_TI_REMAP)

when a name lookup misses in a module context, it retries in the
vmlinux context. module BTF carries kernel types too, and their ids
stay valid across contexts, so the vmlinux id works directly. remap
only runs when the context base chain tops out at vmlinux. a distilled
base (6.12) does not match, so remap is skipped there.

## String base estimation

module BTF strings reference the build time base table. when the
running kernel is built from a different source, those references
shift. private strings (offset beyond the base)
are recovered by voting on name_off - local_off over all types and
members. the winner becomes the string base, verified on 5.10/5.15/
6.1/6.6. strings that reference the base table itself stay shifted
without the distilled table.

## Registry (collaborative registration)

**struct ti_member_desc { const char *name; u32 bit_off; u32 bit_sz; }**

one member description. use compile-time offsetof(x) * 8 for bit_off.

**int ti_reg_struct(const char *name, u32 size,
const struct ti_member_desc *members, u32 n)**

neither entry point is exported by default: a project that links the library in
calls them directly, so the definition is the whole surface. a consumer that loads
this module separately and resolves the names at run time builds with `TI_REG=1`,
which adds `EXPORT_SYMBOL` for both.
register a struct layout. exported, callable from any LKM. 0 on
success, -EINVAL on bad input, -ENOSPC when the table is full.
registered structs are found by ti_type_by_name and all query
functions with a synthetic id.

**void ti_unreg_struct(const char *name)**

remove a registered layout. call it from your module exit.

this is the only layout source for LKMs on no-BTF kernels. it does not
depend on the kernel BTF or on kernel version. verified working on
android12-5.10.

## Module

**struct ti_module { const char *name; unsigned int state;
unsigned long core_base; unsigned long core_size; void *mod; }**

one loaded module. state follows the kernel enum (LIVE 0, COMING 1,
GOING 2, UNFORMED 3). core_base/core_size are zero when the kernel has
no core_layout (6.6+ replaced it with struct module_memory mem[]).

**int ti_mod_lookup(const char *name, struct ti_ctx **out)**

find the captured BTF context of a loaded module. 0 when captured,
-ENOENT otherwise.

**int ti_mod_enum(int (*cb)(const struct ti_module *m, void *arg),
void *arg)**

walk the module list, call cb for each module. return cb's result to
stop early. works on all kernels, with or without BTF.

## Anchor scanning (CONFIG_TI_PUBLIC_ANCHOR)

built with TI_PUBLIC_ANCHOR=1, these primitives are exported for custom
anchor scans.

**int ti_safe_read(void *dst, const void *src, size_t sz)**

fault-safe memory read. zero on success, nonzero on failure.

**int ti_scan_bytes(const void *base, u32 range, const void *sample,
u32 len, u32 *off)**

find a byte pattern in a window.

**int ti_scan_u32(const void *base, u32 range, u32 val, u32 *off)**

find a u32 value in a window.

**int ti_scan_ptrpair(const void *base, u32 range, u32 *off)**

find two equal nonzero pointers next to each other.

**int ti_scan_ptrpair_rev(const void *base, u32 range, u32 *off)**

same, scanning backwards.

**int ti_anchor_set_modname(const char *name)**

the symbol is not exported by default. a project that links the library in carries
the function itself and needs no export, so the definition is the whole surface;
a consumer that loads this module separately and resolves the name at run time
builds with `TI_MODNAME=1`, which adds `EXPORT_SYMBOL(ti_anchor_set_modname)`.

pin the module name used by the struct module anchor. by default the
anchor scans the module mirror for a name and checks it against
init/exit symbols. call this before ti_init to force a known name.
exported.

custom anchor pattern: scan for a known value (string, constant,
pointer pair), verify with cross-checks across several objects, cache
the offset. ti_bootstrap_task and ti_bootstrap_module are reference
implementations. the object you scan must stay mapped; the scan itself
never faults.

## No-BTF kernels (android12-5.10)

android12-5.10 GKI is built without CONFIG_DEBUG_INFO_BTF (android13
and later have it). this means:

- no vmlinux .BTF, so ti_init takes the anchor path and kernel struct
  layout comes from anchor scanning (task_struct five-piece, struct
  module)
- android13-5.10 does have vmlinux BTF, but both 5.10 kernels are
  built without the module BTF option, so modules carry no usable
  btf_data and module capture is unavailable
- the only layout source for later-loaded LKMs is collaborative
  registration (ti_reg_struct), which does not depend on BTF
- arbitrary third-party LKM layouts have no info source and cannot be
  recovered

## DWARF member names (CONFIG_TI_DWARF)

module BTF member names shift when the running kernel is built from a
different source than the module. the module mirror keeps its own
DWARF data, and that data is self contained: member names and offsets
stay valid no matter which kernel the module runs on. with
CONFIG_TI_DWARF, the module context parses it at capture time.

**int ti_dw_capture(struct ti_dw *dw, const void *btf_data)**

find the ELF header by scanning backward from btf_data, read the
section table, apply ET_REL relocations, then walk DWARF4/5 units. on
success the context uses the table as a name fallback. bitfield
members use DW_AT_data_bit_offset (DWARF5 and clang) and
DW_AT_bit_offset (DWARF4) math, both supported.

**int ti_dw_member_off(const struct ti_dw *dw, const char *sname,
const char *member, u32 *bit_off, u32 *bit_sz)**

look up a member offset by name in the DWARF table.

**const char *ti_dw_member_name(const struct ti_dw *dw,
const char *sname, u32 bit_off, u32 bit_sz)**

look up a member name by offset in the DWARF table.

with CONFIG_TI_DWARF, ti_member_off falls back to the table when BTF
misses, and ti_member_at uses the table to rewrite names.

two export levels: CONFIG_TI_DWARF keeps the table internal (the
query functions still use it), CONFIG_TI_DWARF_EXPORT also exports
the three functions above to other LKMs. the captured table lives in
the module context, other LKMs normally reach it through
ti_mod_lookup. ti_dw_capture needs btf_data, which the kernel clears
after module init, so a manual capture only works for a module that
is loading right now.

## Load options (module params)

these module params live in src/main.c. they apply to the resident
type_info.ko. a source-linked LKM (lib only) keeps the defaults and
cannot change them at load time.

ti_cur_pid: current process pid, bootstrap input for the task anchor

ti_ref_pid: second process pid, used to cross-verify the pid anchor

ti_mod_anchor: force the struct module anchor path (testing)

ti_btf_mode: module BTF shape: 0 AUTO, 1 DISTILLED, 2 SPLIT,
3 STANDALONE. AUTO uses the module's btf_base_data when present,
otherwise vmlinux as base.

## Build options

TI_PUBLIC_ANCHOR=1: export the anchor scanning primitives (default off)

TI_FEATURE=1: export ti_type_by_size and ti_type_by_seq (default off)

TI_FUNC=1: function, enum and raw type query (default off)

TI_REMAP=1: retry lookups against vmlinux (default on, TI_REMAP=0 off)

TI_DWARF=1: capture module DWARF member names (default off)

TI_DWARF_EXPORT=1: also export the DWARF functions (implies TI_DWARF,
default off)

TI_MOD_ANCHOR=1: enable the struct module anchor bootstrap
(default off). the module list scan walks every loaded module and
is unsafe on devices with many vendor modules, so it is disabled
by default. consumers that need module offsets without BTF (or
want the anchor as a fallback) opt in; module offsets otherwise
come from BTF only (ti_mod_offs_init), module BTF capture needs
the anchor.

## Limitations

- android12-5.10 has no vmlinux BTF and no module BTF. kernel layout
  comes from anchors, module layout from registration.
- android13-5.10 has vmlinux BTF but no module BTF (the kernel is
  built without the module BTF option), so module capture and DWARF
  do not run.
- android15-6.6 and android16-6.12 modules carry relocation
  placeholders in their debug info, the module mirror keeps the raw
  bytes, so DWARF capture fails there. 6.12 names are fine via the
  distilled base, 6.6 module private names stay shifted.
- DWARF capture runs when a module loads. the kernel clears btf_data
  after module init, so a module that loaded before type_info cannot
  be captured later. load type_info first.
- DWARF only knows module owned structs. kernel structs have no body
  in module DWARF, their names come from vmlinux BTF.
- names that reference the base table (see String base estimation)
  stay shifted on cross source builds.
