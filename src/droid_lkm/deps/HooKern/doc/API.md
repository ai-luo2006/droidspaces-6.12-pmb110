# LKMhook API

kernel hook library. resolves symbols through a pointer-injected
resolver, writes read-only kernel memory through the fixmap slot,
replaces function pointers with tracking, and wraps kprobe and
kretprobe.

## Layout injection

the library never links a layout source. the only kernel layout
capability is the injected resolver in `struct hk_cfg`, typically a
KallRecon wrapper. on old-CFI kernels the wrapper must be a
`__nocfi` function around `kallrecon_klp`, passing the raw pointer
makes the module's indirect call check fail with a CFI panic.

```c
struct hk_cfg {
	unsigned long (*resolve)(const char *name);
};
```

## What a build compiles in

the library is a core and a set of groups, and a consumer links the ones it uses.
the core is `hk` and `hk_ksym`, which everything needs. every other group is one
object, and a group the consumer leaves out is left out of the link entirely:

| group | objects | flag | needs |
|---|---|---|---|
| patch | hk_patch hk_flush | HK_NO_PATCH | core |
| kprobe | hk_kprobe | HK_NO_KPROBE | core |
| kretprobe | hk_kretprobe | HK_NO_KRETPROBE | core |
| inline | hk_inline | HK_NO_INLINE | patch |
| ptr | hk_ptr | HK_NO_PTR | patch |
| cfi | hk_cfi | none | patch |
| lsm | hk_lsm | none | patch |
| sighook | hk_sighook | HK_NO_SIGHOOK | kprobe |
| binder | hk_binder | none | inline |
| redir | hk_redir | HK_NO_REDIR | kprobe, kretprobe, and inline with patch for the text route |

the flag is what keeps the core linkable without the group: it turns the calls the
core makes into that group into nothing, so `hk_exit` and `hk_exit_block` still
compile against a build that has no sighook in it. the flag never removes an entry
point of the group itself, so calling `hk_inline_hook` in a build that left inline
out is a link error and not a call that quietly does nothing. cfi, lsm and binder
are not called by the core at all, which is why they need no flag.

a build without inline has no text route: hk_redir_install answers -EOPNOTSUPP for
HK_REDIR_TEXT, which is the same answer the address route gives to a mechanism the
caller did not link in.

**the notice, and what the library does not check by itself.** a refusal the
library carries on from is reported through one function pointer, and the build
installs none to get the default:

	enum hk_notice {
		HK_NOTICE_FREE_BEFORE_DISABLE = 0,
		HK_NOTICE_RELO_TABLE,
	};
	typedef void (*hk_notify_fn)(enum hk_notice what, const char *detail);
	void hk_notice_set(hk_notify_fn fn);

the default prints with pr_warn, the level this library uses for a condition
it refused and carried on from, which is the report a plain build gives.
`hk_notice_set(NULL)` silences every notice, and a caller that wants to count them
or trip its own assertion installs its own function. the notice runs where the
refusal happens, in process context, and it must not sleep.

**int hk_relo_check(void)** reconciles the relocator. every instruction class is
emitted once into a scratch trampoline and the words it wrote are compared with
the length table, which is what turns an address inside the window into the
address the same instruction has in the trampoline: an emitter that grew without
the table following would put every relocation after it one instruction off. the
library never runs this by itself, because the table is written next to the
emitters, so a build that wants it proven calls it: a test does, and a consumer
that changed the relocator can. it allocates nothing, takes no lock and does not
sleep. 0 when every class agrees, -EIO when one does not, and the notice names it.

the class tables themselves are held by the compiler either way: the masks, the
patterns and the lengths are three tables indexed by one enum, and a class added
without its three entries is a build error. that check costs nothing and is
always on; the emitters are the part the compiler cannot see, and that part is
what hk_relo_check is for.

## Lifecycle

**int hk_init(const struct hk_cfg *cfg)**

the one call that starts everything. the cfg is copied by value, the
caller does not need to keep it alive. 0 on success. -EINVAL when cfg
or the resolver is bad. -EALREADY when already initialized, call
hk_exit first.

**void hk_exit(void)**

restores every tracked pointer, removes every installed probe and
clears state. safe to call multiple times.

**unsigned long hk_resolve(const char *name)**

query through the injected resolver. 0 when unset or unresolved. when
the injected resolver has nothing, or the library is between `hk_exit`
and the next `hk_init`, the kernel symbol table is walked once for a
`name$type` entry, which is what a clang CFI build calls the body of a
function whose address was taken, and that body is returned.

**void hk_exit_block(void)**

the end of a module's exit path, for a module whose kernel text still
branches into a hook. it never returns while a hook is live: it retries
`hk_inline_exit`, `hk_ptr_exit` and `hk_kretprobe_exit` every 60 seconds
and only returns once every count is zero, which is `hk_inline_pending`,
`hk_ptr_pending`, `hk_kprobe_pending`, `hk_kretprobe_pending`,
`hk_redir_pending` and `hk_redir_inflight`. the last one is what covers a
call that is inside a replacement right now: that code is this module's
and the reference it holds is what keeps the kernel from unmapping it.
rmmod unmaps module text and data as soon as the exit path returns, so a
live detour without this points into freed memory.

## Symbols

**int hk_ksym_register(const struct hk_sym *table)**

batch resolve a NULL terminated table, optional fallback name per
entry, values stored through `storage`. -ENOENT when any required
entry fails, the count of missing ones is logged.

```c
struct hk_sym {
	const char *name;
	const char *fallback;
	void **storage;
	bool required;
};
```

## Patch

writing kernel text. a call names the mechanism it wants, the library keeps no
state about which one is right, and a fallback, a retry or a test before the
write is written by the caller:

**int hk_write_kernel(void *dst, const void *src, size_t len)** calls the
kernel's own aarch64_insn_patch_text under cpus_read_lock, up to
`HK_PATCH_INSNS_MAX` instructions. it parks the cores and maps through the
kernel's own frame, so it assumes nothing about the headers this module was
built with.

the kernel sends every address that is not its own text to vmalloc_to_page, and
the form of that function which predates 5.13 answers a block mapped address with
a null page and then trips its own BUG_ON. the entry point asks the kernel's
translation itself for such an address, which is a page or a warning and never the
BUG_ON, and returns -EOPNOTSUPP with a line naming hk_write_fixmap when the kernel
cannot take it. nothing panics the machine for it.

the descriptor also names where the store goes: HK_SLOT_LIBRARY (the slot this
library owns), HK_SLOT_KERNEL (FIX_TEXT_POKE0, the one the kernel's patcher maps
through, written under stop_machine) and HK_SLOT_OWN (the target's own entry, made
writable with the write bits alone and put back the same way, so the store still
dirties the page and no fixmap is touched). mm names an address space of the
caller's own, flush its invalidation, mkwrite its way of granting the write, and
pte_of the way it finds that entry, a null one being the library's own walk.
**int hk_write_window(void *dst, const void *src, size_t len, const struct hk_write
*w, int (*fn)(void *ctx, void *addr, size_t len), void *ctx)** runs that window and
lets the caller store its own way, once per page over the range.

a hook that lands on a guarded page records what it had to do in
`struct hk_inline.use_br_x17`: true means the jump back goes through `br x17` with
the guarded bit taken off the page, false means it fell back to `ret x17`, which
needs no landing pad at all. the library keeps the state and writes no log of its
own, so a project that wants one reads the field after the hook is installed.

**int hk_write_run(void *dst, const void *src, size_t len, const struct hk_write
*w)** is the same write with every mechanism named by the caller: the translation
(hk_va_walk_pa, hk_va_to_pa, or one of its own, a null one meaning the library's
walk), the slot (HK_SLOT_LIBRARY, the library's own, or HK_SLOT_KERNEL, the one the
kernel's patcher maps through), whether a store that cannot land is an error, and
whether the target is read back afterwards. a zeroed descriptor is the library's own
route with no read back. **int hk_write_poke(...)** is the kernel slot with the
fault tolerant store and the read back, which is the route a platform that watches
fixmap use expects to see.

every write returns 0 only when the bytes are in the target, -ENOENT when the
address cannot be translated, -EIO when they were written and did not land, and
-EOPNOTSUPP when the route does not fit the address. the library writes no log of
its own, so a caller that wants one reads the return value.

**int hk_write_fixmap(void *dst, const void *src, size_t len)** writes byte
wide, per page, through a slot this build maps itself, and translates with the
library's own walk, which is the only route that answers a block mapped address
correctly on every kernel. **int hk_write_fixmap_by(void *dst, const void *src,
size_t len, unsigned long (*to_pa)(unsigned long va))** is the same write with the
translation named by the caller: hk_va_to_pa asks the kernel, hk_va_walk_pa walks
the tables here, and a null function means the walk. a caller that wants the
destination checked before the store asks **hk_va_maps(unsigned long va,
unsigned long pa)** and decides what to do with the answer.

**int hk_write_direct(void *dst, const void *src, size_t len)** stores where
the kernel already mapped the page writable. **bool hk_va_writable(unsigned
long va)** answers whether that is the case, and the library does not repeat
the test.

**int hk_write_one(void *dst, u32 insn)** writes one instruction through
aarch64_insn_write and is the only path that may be called from an atomic
context.

**int hk_write_text(void *dst, const void *src, size_t len)** is the path
hk_init was configured with, `struct hk_cfg.write`, or the kernel primitive
when that is NULL. every write inside the library, and the calls below, take
this one.

every primitive cleans the data cache over the range and invalidates the
instruction cache, leaves nothing mapped on any path including the failing
ones, and returns 0 or a negative errno. all of them are process context, the
calls sleep.

**unsigned long hk_va_to_pa(unsigned long va)** translates the way the kernel's
own patcher does: image addresses arithmetically, the linear map arithmetically,
everything else through the kernel's vmalloc_to_pfn. a frame the kernel does not
own is reported as 0 rather than handed back, which matters because that path
answers a block mapped address with a null page before 5.13. **unsigned long
hk_va_walk_pa(unsigned long va)** walks the tables here, asking none, bad and leaf
in that order at every level, so a 2 MB or 1 GB block answers as correctly as a
page. both return 0 when the address cannot be translated.

**int hk_va_kind(unsigned long va)** answers which family an address belongs to:
image text, image data (rodata and data, which the kernel's own patcher sends to
vmalloc_to_page), the linear map (where read only data has a read only alias as
well), vmalloc and module memory, or user. the family is what decides how a write
has to be made, so a caller that picks its own route asks this first rather than
guessing from the value; the library never picks a route from it by itself.

the route that writes through the target's own entry needs that entry, and a
mapping built from a block descriptor has none: the kernel image is mapped that
way at the 2 MB level, so that route answers HK_SLOT_OWN destinations in image
rodata and image text with -EOPNOTSUPP instead of writing somewhere else, which
is the same freedom pte_of hands a caller that finds entries its own way.

**bool hk_va_writable(unsigned long va)** answers whether the mapping allows a
store, read from the descriptor of the level that maps the address, and **bool
hk_va_maps(unsigned long va, unsigned long pa)** answers whether the address is
mapped and carries that frame. both are leaf aware, so a block mapped address is
answered instead of being called unmapped.

**int hk_patch_write(void *dst, unsigned long val)** writes one unsigned long
through the configured path, which is what a table entry needs. -EINVAL on a
bad argument, -ENOENT when a symbol the path needs is absent, -EIO or -EFAULT
when the address cannot be reached.

**int hk_patch_prepare(struct hk_patch_set *set, struct hk_patch_hook *hook)**

the precheck of a transaction. nothing is written: the destination is
translated, the current bytes are read through the nofault helper into
`hook->orig` and checksummed into `hook->checksum`, and the hook joins
the set. -EALREADY on a hook prepared twice, -EINVAL on a bad argument
or a length over `HK_PATCH_INSNS_MAX` words, and the translation codes
above. both structs are caller owned.

**int hk_patch_commit(struct hk_patch_set *set)**

every site is compared against the bytes the prepare saw before the
first write, so a site another writer took is -EAGAIN with nothing
changed. a write that fails after that point puts the sites already
written back, so a set is all in or all out, and the failing code is
returned. the writes take the configured path.

**int hk_patch_rollback(struct hk_patch_set *set)**

restore the sites the set has active. the first failure is returned, a
hook that could not be restored stays active for a later call.
**void hk_patch_set_init(struct hk_patch_set *set)** and **void
hk_patch_set_release(struct hk_patch_set *set)** start and empty a set,
release does not write, and **unsigned int hk_patch_set_count(const
struct hk_patch_set *set)** counts the hooks in it.

the hook set is what a hot replacement is built on: prepare every new
hook, commit the set, then unhook the old ones.

## Inline hook

**int hk_inline_hook(struct hk_inline *h, const char *sym, const char
*wrapper_sym)**

replace the entry of sym with a 20 byte detour and build an internal
exec trampoline. the wrapper is resolved from wrapper_sym and invoked
through an absolute LDR X17 + RET X17 jump, so there is no plus or minus 128 MB
range limit. h->orig points to the trampoline entry, call it from the
wrapper to run the original function.

the library records the window of every live hook. an install whose
window overlaps one is refused with -EBUSY, the log line names both
symbols, nothing is written. hk_inline_probe answers the same condition
before an install is attempted.

installs are serialized by a mutex held across trampoline allocation,
relocation and the text writes, so two hooks cannot interleave and a
restore cannot run inside an install. the write itself is still not
atomic with respect to a core executing the target, the lock removes
the second writer only, so a hot path other cores run during
installation still belongs to the kprobe entries. stop_machine is not
used: this path allocates and resolves symbols and a stop_machine
callback must do neither, so a prepare and commit split has to come
before the callback can carry the last write.

the detour covers HK_INLINE_PATCH_LEN bytes from the entry, so the
entry needs that much room of its own. a shorter symbol shares its
window with the next function, hk_inline_probe reports the length.

wrapper_sym must be a global symbol (LTO localizes static ones and
drops the names from kallsyms). addresses come from the resolver, not
from &func.

trampoline layout:

```
+0x00  bti jc
+0x04  ldr x17, #8
+0x08  ret x17
+0x0c  wrapper low
+0x10  wrapper high
+0x14  bti jc          trampoline entry, h->orig
+0x18  saved window    relocated original instructions
+...   ldr x17 + ret   jump back to func+window
```

```c
struct hk_inline {
	const char *name;
	unsigned long addr;
	unsigned long orig;
	void *mem;
	size_t mem_size;
	u32 window;
	bool disabled;
	bool pending;
	bool use_br_x17;
	struct hk_inline_guard guard[HK_INLINE_GUARD_MAX];
	u32 guard_count;
	u8 saved[HK_INLINE_ENTRY_MAX * 4];
};
```

the continuation after the patched window is reached with an indirect
branch, so the pages that hold one and carry the guarded attribute have
it cleared for the install and put back when the trampoline is freed,
recorded in `guard` and counted by `guard_count`. when the attribute
cannot be cleared the continuation uses `RET X17` instead of `BR X17`,
which is a return and needs no landing pad, and `use_br_x17` reports
which of the two is in the trampoline.

**int hk_inline_disable(struct hk_inline *h)**

restore the original entry but keep the trampoline alive. use before
waiting for in-flight calls. runs under the same install lock. the
window is released with the restore, a later install can take it. -EIO
when the write fails, the hook stays live, the window stays held and
the trampoline stays allocated, call it again.

**void hk_inline_free(struct hk_inline *h)**

free the trampoline after disable and release the window. safe to call
on a zeroed hook. calling it before the disable is a programmer error
and not a runtime condition: the detour is still the entry, so the free
is refused and the trampoline stays, which leaks one allocation instead
of leaving the entry pointing at freed code. the refusal is quiet in a
plain build and reported as a warning, inside the notice
`HK_NOTICE_FREE_BEFORE_DISABLE`: a caller that wants to hear about it keeps the
default, and one that does not silences it with hk_notice_set(NULL) or installs
its own function.

**void hk_inline_unhook(struct hk_inline *h)**

the three stage unload: disable the entry, wait one `synchronize_rcu_tasks`
grace period so a core that already fetched the detour is out of the
trampoline, then free it. a hook whose entry could not be restored is
parked, the trampoline is kept and `hk_inline_pending` counts it, and
`hk_inline_exit` retries every parked hook. that count is what
`hk_exit_block` gates the module exit on.

**int hk_inline_probe(const char *sym, struct hk_inline_probe *out)**

read only half of hk_inline_hook, for a caller that wants to judge a
live hot path before it is patched. it resolves sym, follows the same
branch chain, reads the entry window through the nofault helper and
runs the relocator over a copy of it. it writes nothing, allocates
nothing, does not sleep and never allocates exec memory. 0 when the
report was filled, -EINVAL on a NULL argument.

```c
struct hk_inline_probe {
	unsigned long addr;	/* resolver result, 0 when unresolved */
	unsigned long target;	/* entry a hook would patch, chain followed */
	u32 patch_len;		/* bytes the detour overwrites there */
	enum hk_inline_state state;
	const char *reason;	/* static string, the caller never owns it */
	unsigned long collide_addr;	/* start of the window in the way */
};
```

```
HK_INLINE_UNRESOLVED   sym did not resolve, addr is 0
HK_INLINE_UNREADABLE   the entry window could not be read
HK_INLINE_UNSUPPORTED  the relocator refuses the window, an operand in it
                       points back into the window, or addr is not a
                       kernel address
HK_INLINE_HOOKED       a jump stub sits in the window: br, brk, ldr plus br,
                       or a movz movk chain ending in br
HK_INLINE_PATCHSITE    an ftrace patch site: mov x9, x30 at the entry, with
                       nop or bl behind it
HK_INLINE_BRANCHED     a direct b in the entry, target is where a hook would
                       write, the end of the chain when the walk follows it
                       and the entry itself when it does not
HK_INLINE_PLAIN        no known stub, the relocator accepts the window
HK_INLINE_COLLISION    a live hook owns part of the window, an install
                       would return -EBUSY, collide_addr is its start
```

the collision answer comes from the window table the installs keep, so it
holds for the hooks already live when the probe runs.

every state except plain means the entry should not be patched as it
stands. plain is signature based: an adrp add br stub, a patch that
keeps the first instructions, and an ftrace site on a kernel whose
ftrace patches a bare nop are not recognised, so plain means no known
stub was found, it is not proof of an untouched prologue.

## Pointer replacement

**int hk_ptr_hook(void **slot, void *replacement, void **orig_out)**

read the pointer at slot, replace it, record the original in the
tracking list. one lock is held across the duplicate check, the read,
the swap and the record, so two hooks cannot interleave and lose an
original; the swap itself is a single aligned word store, a core that
reads the slot sees the old pointer or the new one. the list has no
fixed capacity, the walk over the live hooks costs what the old 16 slot
table scan did. the tracking entry is allocated, so this runs in
process context. -EEXIST when the slot is already hooked. -EFAULT when
the slot cannot be read. -EIO when the write fails. -ENOMEM when the
tracking entry cannot be allocated, there is no full table any more.
the original is written to orig_out on success.

**void hk_ptr_unhook(void **slot)**

restore the recorded original. silent when the slot is not hooked.

**void hk_ptr_exit(void)**

restore every tracked slot. called by hk_exit.

## Kprobe

kprobe is the entry for a target other cores can execute during
installation. register_kprobe arms its breakpoint under text_mutex and
cpus_read_lock, so a hot path like device_add can be probed while the
inline primitive must not patch it. the price is the handler contract:
a probe observes a call, it does not replace the function, and calling
the target from a handler re-enters the same probe. use kprobe for a
hot target, for an entry window hk_inline_probe reports as
HK_INLINE_PATCHSITE, an ftrace patch site the relocator would lose, and
for a window it reports as HK_INLINE_UNSUPPORTED.

**int hk_kprobe_install(struct hk_kprobe *h, const char *sym, kprobe_pre_handler_t pre)**

register a kprobe on sym with the given pre handler. the resolved
address lands in h->orig. register_kprobe and unregister_kprobe are
resolved at runtime through `__nocfi` wrappers, some GKI builds trim
the exports, a missing symbol fails with -ENODATA. a refused install
logs the symbol, the kernel code and the gate that fired, and returns
the kernel code: -EINVAL for every address gate, -ENOENT for a symbol
that does not resolve. -ENOMEM when the tracking node cannot be
allocated, nothing is registered then, there is no ceiling on the
number of live probes. hk_kprobe_remove logs when the probe cannot be
unregistered again, a probe left armed on module text is a use after
free, and a second remove of the same hook is a no op.

```c
struct hk_kprobe {
	struct kprobe kp;
	unsigned long orig;
};
```

**int hk_kprobe_install_at(struct hk_kprobe *h, unsigned long addr, kprobe_pre_handler_t pre)**

the same for a target the caller holds as an address: a syscall table
entry it read itself, or a function whose name it does not want the
resolver to see. addr has to be kernel or module text, which is the same
gate the kernel asks, and the tracking, the count of live probes and the
exit path are the ones hk_kprobe_install uses. h->orig is addr.

**int hk_kprobe_check(const char *sym, struct hk_kprobe_report *out)**

ask the address gates of register_kprobe before an install and name the
one that would refuse the symbol, the way hk_inline_probe judges an
inline target. nothing is written, nothing is allocated and it does not
sleep. 0 when the report was filled, -EINVAL on a NULL argument.

```c
struct hk_kprobe_report {
	unsigned long addr;	/* resolver result, 0 when unresolved */
	enum hk_kprobe_state state;
	const char *reason;	/* static string, the caller never owns it */
};

HK_KPROBE_UNRESOLVED   the resolver did not find the symbol, addr is 0
HK_KPROBE_UNSUPPORTED  not text, or a cfi preamble symbol
HK_KPROBE_BLACKLISTED  kprobe_blacklist, __kprobes or noinstr text, the
                       reason names hk_kprobe_clear_blacklist
HK_KPROBE_PATCHSITE    a recorded ftrace call site, refused on a kernel
                       without kprobes on ftrace, arm64 has none
HK_KPROBE_OK           no address gate matched
```

**int hk_kprobe_check_at(unsigned long addr, struct hk_kprobe_report *out)**

the same gates on an address, for a target that never was a symbol. a
report on an address never carries HK_KPROBE_UNRESOLVED or the cfi
preamble state, both of those are about a name.

the gates are the kprobe ones for a kretprobe too, register_kretprobe
registers a kprobe. HK_KPROBE_OK is not a promise, an install can still
fail on a kernel symbol the resolver cannot reach or on a tracking node
that cannot be allocated.

**handler helpers**

arm64 carries the first eight arguments of a call in x0 to x7 and the
return value in x0. hk_regs_arg and hk_regs_set_arg read and write an
argument at an entry, hk_regs_ret and hk_regs_set_ret read and write
the return value where a kretprobe handler runs, hk_regs_ip returns the
pc, hk_regs_lr returns the link register, and hk_regs_skip steps the pc
over the probed instruction for a pre handler that returns 1.

the replacement idiom for a hot target is hk_regs_return, it parks the
pc on the link register so the target returns at once with x0 as the
result and its prologue never runs, which is as close to a wrapper as a
probe gets:

```c
static int pre(struct kprobe *p, struct pt_regs *regs)
{
	hk_regs_set_ret(regs, my_device_add(hk_regs_arg(regs, 0)));
	hk_regs_return(regs);
	return 1;
}
```

the other idiom is hk_regs_redirect, which hands the call to another
function instead of running the target. the replacement is entered with
the argument registers of the target, on the caller stack of the target,
and its return goes to the caller of the target, so what it returns is
what the target returns. it is reached by the return from the breakpoint
exception and not by a branch, so no cfi check and no bti landing pad
stands at its entry, and the target does not run unless the replacement
calls it. a handler that redirects returns 1 like one that parks the pc,
and reaching the original again is the address of the probed entry, which
is what hk_redir below is about.

**int hk_kprobe_remove(struct hk_kprobe *h)** unregisters and untracks, 0
when the probe is gone and -ENODATA when unregister_kprobe cannot be
reached and the probe is still armed, a second remove being a no op.
**void hk_kprobe_exit(void)** removes every tracked probe, called by
hk_exit. **unsigned int hk_kprobe_pending(void)** counts the probes that
sweep could not take back: their handler is still reached from kernel
text, so a module that owns one must not be unmapped, and hk_exit_block
counts them.

**int hk_kprobe_clear_blacklist(void)**

resolve the global `kprobe_blacklist`, save every active entry and
zero its start/end so protected functions can be probed. returns 0 on
success, -ENOENT when the list symbol is missing, -ENOMEM when the
save array cannot be allocated. call this explicitly before installing
probes on blacklisted symbols, it is not called by hk_kprobe_install.

**void hk_kprobe_restore_blacklist(void)**

restore the saved start/end addresses and free the save array. called
automatically by hk_kprobe_exit, safe to call manually before exit.

## Redirect

running another function in place of a kernel function. the caller names
the target, the replacement and the mechanism, and the library keeps no
opinion about which mechanism suits which target: hk_inline_probe and
hk_kprobe_check answer that before anything is written. a refused install
returns what its route reports, -EINVAL for a NULL argument and
-EOPNOTSUPP for a route that cannot take what it was given.

```c
enum hk_redir_how {
	HK_REDIR_TEXT = 0,	/* the entry is written, the target must not be running */
	HK_REDIR_KPROBE,	/* the entry keeps its bytes, a breakpoint carries the call */
};

struct hk_redir {
	enum hk_redir_how how;
	enum hk_redir_guard guard;	/* named before the install, zero is the task marker */
	bool pin;			/* named before the install, see below */
	const char *name;	/* the target as it was named, null for an address */
	unsigned long addr;	/* the entry a call lands on */
	unsigned long orig;	/* what a replacement calls to reach the original */
	unsigned long repl;	/* what a redirected call runs instead */
	bool live;
	bool pending;
	struct hk_kretprobe retprobe;	/* the completion signal, pinned hooks only */
	struct xarray tasks;		/* the per task marker, task guard only */
	union {
		struct hk_inline text;
		struct hk_kprobe kprobe;
	} u;
};
```

**int hk_redir_install(struct hk_redir *r, const char *sym, const char *repl_sym, enum hk_redir_how how)**

both sides by name, resolved here, because a branch is written to the
address the resolver answers, cfi suffix included, and because the text
route judges and relocates the entry before it writes it. orig is the
trampoline hk_inline builds for HK_REDIR_TEXT and the probed entry itself
for HK_REDIR_KPROBE.

**int hk_redir_install_at(struct hk_redir *r, unsigned long addr, unsigned long repl, enum hk_redir_how how)**

both sides by address, nothing resolved, so a syscall table entry the
caller read itself is a target like any other. HK_REDIR_KPROBE only,
HK_REDIR_TEXT answers -EOPNOTSUPP because it patches an entry it has to
judge by name. addr has to be the address a call lands on: on a cfi
build the plain symbol of a function is a stub and a call inside the same
module lands behind it, so what a table entry holds or what
hk_redir_install resolved is a target and a plain &func is not.

**int hk_redir_remove(struct hk_redir *r)** takes it back. the text route
disables the entry, waits one tasks rcu grace period and frees its
trampoline, and a hook whose entry could not be restored stays live and
counted by hk_inline_pending. the kprobe route unregisters the probe,
waits for a core still inside the replacement, then drops the guard, and
a probe that could not be unregistered is reported here and counted by
hk_kprobe_pending.

**the marker of the kprobe route.** a replacement reaches the original
by calling hk_redir.orig, which is the probed entry itself: the
breakpoint fires again and the handler has to let that call through
instead of redirecting it a second time. which calls are let through is
the caller's choice, named in hk_redir.guard before the install.

HK_REDIR_GUARD_TASK is the per task marker, and the zero value. the
handler sets it before it parks the pc and the replacement clears it
through hk_redir_leave. a replacement that never clears it stops being
redirected for that task and the original runs from then on, which is a
lost hook and not a crash. it costs one xarray entry per task that passes
and needs the leave call in every replacement.

HK_REDIR_GUARD_CALLER keeps no state: the handler reads the return address
and lets a call from the module this library was built into through. there
is nothing to clear and nothing to forget, and a call the owning module
makes on purpose is let through as well, which is the trade against the
task marker.

the text route needs no marker, its trampoline is a copy of the entry, and
naming either pin or the caller marker for it is refused with
-EOPNOTSUPP rather than ignored.

**the pin, kprobe route only.** with hk_redir.pin set before the install,
a redirected call holds a reference on the module this library was built
into, taken in the handler and dropped by a kretprobe the library arms on
the replacement itself. the kernel's own delete_module refuses to unload a
module whose reference count is not zero, so the module cannot be freed
under a call that is still running, and no gate of this library is what
decides it. a pin that cannot be completed is refused instead of being
dropped: -ENODATA when try_module_get or module_put cannot be resolved,
the kernel code when the completion signal cannot be armed. it costs one
more trap per redirected call, and the count belongs to the module rather
than to one hook, because the module is what must not be freed.

**unsigned int hk_redir_pending(void)** counts the redirects that could
not be taken back, and **unsigned int hk_redir_inflight(void)** reports the
calls that are inside a replacement right now, pinned hooks only. both are
counted by hk_exit_block.

```c
__nocfi long my_target(int a)
{
	long ret = my_orig(a);		/* my_orig is r->orig */

	hk_redir_leave(&r);
	return ret + 1;
}
```

the replacement is entered by the return from the breakpoint exception
with the registers of the target, so give it the prototype of the target
and mark it __nocfi like every other function of a module that is reached
from a path the compiler did not build.

## Kretprobe

**int hk_kretprobe_install(struct hk_kretprobe *h, const char *sym, kretprobe_handler_t handler)**

register a kretprobe on sym. the handler signature is the kernel one
(struct kretprobe_instance, struct pt_regs), it runs after the target
returned and the return value is in x0, so hk_regs_set_ret rewrites it,
which a kprobe cannot do. the kernel writes the registers back after
the handler. register_kretprobe and unregister_kretprobe are runtime
resolved like the kprobe ones. the address gates are the kprobe ones,
so hk_kprobe_check answers for a kretprobe target, and a refused
install logs the symbol, the kernel code and the gate. installs are
tracked in a list like the kprobe ones, no ceiling and no -ENOSPC,
-ENOMEM when the tracking node cannot be allocated. the target has to
return, a function that never returns leaks instances until maxactive
runs out and the probe then misses returns instead of failing the
install.

**int hk_kretprobe_install_ex(struct hk_kretprobe *h, const char *sym, kretprobe_handler_t entry, kretprobe_handler_t handler, size_t data_size)**

the entry and return pair. the entry handler runs at the call, returns
1 to accept the instance and can fill ri->data with data_size bytes,
which the return handler reads back from the same instance.

```c
struct hk_kretprobe {
	struct kretprobe rp;
};
```

**int hk_kretprobe_install_at(struct hk_kretprobe *h, unsigned long addr, kretprobe_handler_t handler)**

the same for a target the caller holds as an address, which is what a
function of its own needs: a completion signal has to sit on the
replacement, and a replacement is a pointer, not a name. the gates, the
tracking and the exit path are the ones hk_kretprobe_install uses.

**int hk_kretprobe_remove(struct hk_kretprobe *h)** and **void
hk_kretprobe_exit(void)** mirror the kprobe ones, including the log line
when the probe cannot be unregistered again, and **unsigned int
hk_kretprobe_pending(void)** counts the ones the sweep could not take
back, the counterpart of hk_kprobe_pending.

## Binder trace

**int hk_binder_init(const struct hk_binder_callbacks *cb)**

install inline hooks on binder_ioctl,
binder_alloc_copy_user_to_buffer and binder_transaction (optional).
symbols are resolved with a CFI `$` suffix fallback. the callbacks are
invoked before the original function. init also calls hk_cfi_bypass()
so old-CFI indirect calls through file_operations do not panic.
returns 0 on success, -EINVAL on NULL callbacks, -ENOENT when the
required binder symbols are missing.

**void hk_binder_exit(void)**

set draining, disable all hooks, synchronize_rcu_tasks (resolved at
runtime, missing symbol is a no-op), wait for in-flight wrappers and
free the trampolines.

```c
struct hk_binder_callbacks {
	void (*ioctl)(void *priv, void *filp, unsigned int cmd,
		      unsigned long arg);
	void (*copy_from_user)(void *priv, void *alloc, void *buffer,
			       unsigned long buffer_offset,
			       const void __user *from, size_t bytes);
	void (*transaction)(void *priv,
			    const struct hk_binder_transaction_event *ev);
	void *priv;
};
```

## LSM hook

LSM hook is an optional component compiled with `HK_LSM=1`. it
replaces existing LSM hook slots at runtime. Type_info is vendored
under deps/Type_info and injected through a pointer layout, so the
library does not link Type_info directly.

```c
struct hk_lsm_layout {
	unsigned long (*resolve)(const char *name);
	int (*type_by_name)(const char *name, u32 *id);
	int (*member_off)(u32 id, const char *member, u32 *bit_off,
			  u32 *bit_sz);
	u32 (*type_size)(u32 id);
};
```

**int hk_lsm_init(const struct hk_lsm_layout *layout)**

store the injected Type_info layout. must be called before any
hk_lsm_hook.

**int hk_lsm_hook(struct hk_lsm_hook *hook)**

find the LSM slot whose current handler matches target_name and
replace it with replacement. supports legacy security_hook_heads
(< 6.12) and static_calls_table (>= 6.12).

**void hk_lsm_unhook(struct hk_lsm_hook *hook)**

restore the original handler and untrack the hook.

**int hk_lsm_register(struct hk_lsm_hook *hook)** and **void
hk_lsm_unregister(struct hk_lsm_hook *hook)** are aliases of
hk_lsm_hook / hk_lsm_unhook.

**void hk_lsm_exit(void)**

restore all tracked hooks and clear the injected layout.

```c
#define HK_LSM_HOOK_INIT(member, target_symbol, replacement_fn, off) \
	{                                                              \
		.head_name = #member,                                  \
		.target_name = target_symbol,                          \
		.head_offset = offsetof(HK_LSM_HEADS_TYPE, member),    \
		.hook_offset = offsetof(struct security_hook_list, hook.member),\
		.replacement = (void *)(replacement_fn),               \
		.offset = off,                                         \
	}
```

when a Type_info layout is injected, hk_lsm uses layout->member_off
to resolve security_hook_list and lsm_static_call offsets at runtime,
so compile-time offsetof values are only a fallback.
