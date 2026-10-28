# Kerncall API

kernel syscall channel library. hijacks a free `ni_syscall` slot in
`sys_call_table` and routes userland `syscall(nr, key, cmd, ...)`
calls to a dispatch callback. can also patch arbitrary existing
syscall slots. optionally hosts a `sys_enter` tracepoint dispatcher
for runtime syscall hooks that never touch the table.

## Layout injection

the library never links a layout source. all kernel layout
capabilities come from a pointer-injected `struct sc_layout`,
typically wrapping type_info (BTF first, anchor fallback) plus
KallRecon for symbol resolution. any implementation can be swapped
in, cross-source compatible.

```c
struct sc_layout {
	unsigned long (*resolve)(const char *name);
	int (*pgd_off)(u32 *out);
	int (*find_slot)(void);
};
```

- `resolve`: symbol name to address. used for `sys_call_table`,
  `__arm64_sys_ni_syscall` (+ `.cfi_jt`) and the tracepoint
  dispatcher (`__tracepoint_sys_enter`, `syscall_regfunc`,
  `syscall_unregfunc`, `tp_stub_func`, `tracepoint_srcu`,
  `synchronize_srcu`). the same pointer is handed to HooKern, which
  resolves its own patch and hook symbols through it. the wrapper
  must be a `__nocfi` function around `kallrecon_klp`, passing the
  raw pointer makes the module's indirect call check fail with a
  CFI panic on old-CFI kernels
- `pgd_off`: byte offset of `mm_struct.pgd`. reserved, the library
  itself does not use it. kept in the layout struct so consumers can
  carry it for their own page table work
- `find_slot`: optional empty-slot discovery method supplied by the
  layout library. see Slot discovery

## Lifecycle

**int sc_init(const struct sc_cfg *cfg)**

the one call that starts everything. initializes HooKern with the
layout resolver, resolves symbols, discovers a slot, patches in the
handler. 0 on success.
-EINVAL when cfg, key or layout resolver is bad.
-EALREADY when the channel is already initialized, call sc_exit
first.
-ENODATA when a required symbol cannot be resolved.
-EBUSY when no slot is found. -EIO when the patch fails.
not reentrant: a second sc_init while the channel is active fails
with -EALREADY.

the syscall table write is HooKern's, so sc_init initializes it with
the same resolver. a consumer that already initialized HooKern keeps
its own configuration and the -EALREADY of that call is accepted.
the library never calls hk_exit, the lifetime of the hook library
stays with the module that owns it.

with `cfg->no_patch` set, sc_init only resolves the layout
(symbols + pgd ready) and returns without discovering or patching
a channel slot. consumers use this to build their own handler on
top of the patch API, keeping the built-in channel (and its auth
error codes) out of the syscall table. `tp_enable` still works
under no_patch, giving a channel-free tracepoint dispatcher.

**void sc_exit(void)**

restores every patched slot, unregisters the tracepoint dispatcher
if enabled, and unmarks processes. safe to call multiple times.

**int sc_get_slot(void)**

the channel syscall number. -1 before sc_init or after sc_exit.

## Callbacks (struct sc_cfg)

```c
struct sc_cfg {
	const struct sc_layout *layout;
	long (*dispatch)(long cmd, const struct pt_regs *regs, void *priv);
	int (*find_slot)(void);
	char key[SC_KEY_MAX];
	void *priv;
	bool no_patch;
	/* identity callbacks, all optional */
	bool (*is_manager)(void);
	bool (*is_domain)(const struct task_struct *p);
	bool (*is_allow_uid)(uid_t uid);
	/* command table and replay guard */
	const struct sc_cmd *cmds;
	unsigned int ncmds;
	bool replay_enable;
	unsigned long replay_ttl_ms;
	u64 (*replay_sid)(const struct pt_regs *regs, void *priv);
	/* tracepoint dispatcher (CONFIG_KERNSC_TP) */
	bool tp_enable;
	bool tp_mark_all;
	void (*tp_mark_cb)(struct task_struct *p, bool on);
	void (*tp_on_enter)(int id, struct pt_regs *regs);
	unsigned int tp_tier_mask;
	unsigned int (*tp_tier_cb)(const struct task_struct *p, void *priv);
};
```

the callbacks are the customization surface. every behavior is
overridable: slot discovery, command handling, and through the
patch API the syscall table itself.

**dispatch**

command handler. called from the syscall handler after key
authentication and root check. this is where the consumer
implements its own syscall injection logic: parse commands from
regs, act on them, return a long result which becomes the syscall
return value. `priv` is the cfg.priv pointer, passed through
unchanged. may be NULL, then all non-HELLO commands return
-ENOSYS.

**find_slot**

consumer supplied slot discovery. returning -1 makes sc_init fail
with -EBUSY. see Slot discovery for the selection order. consumers
that only want to find an empty slot can supply their own method
here, or leave it unset to use the library default.

**key**

authentication key, any non-empty string chosen by the caller.
the same string is expected from the userland side as the first
syscall argument.

**priv**

caller supplied context, forwarded to dispatch.

**is_manager, is_domain, is_allow_uid**

identity sources for the permission grades, all optional and all
answering false when unset. manager is a process property, domain is
read from a task, allow uid takes the real uid of the caller. which
identity a manager is, or which domain a root process has to be in,
stays with the consumer: the library owns no policy and links no
policy source.

**cmds, ncmds**

consumer command table with a permission grade per command, see
Command table and permission grades.

**replay_enable, replay_ttl_ms, replay_sid**

session check for replay and reorder rejection, see Replay guard.

## Tracepoint dispatcher (CONFIG_KERNSC_TP)

a `sys_enter` tracepoint based dispatcher. one shared `ni_syscall`
slot is patched with a dispatcher handler; the tracepoint redirects
only hooked syscalls to it, and an in-module table routes them to
per-syscall hooks. hooks register and unregister at runtime without
writing the read-only syscall table. the advantage over `sc_patch`
is one table write for any number of hooks, at the cost of marking
processes and passing every marked syscall through the tracepoint.

**lifecycle**

set `tp_enable` in cfg and pass it to sc_init. the dispatcher finds
its own free slot (excluding the channel slot and patched entries),
patches it, registers the tracepoint and marks processes. all of it
is torn down by sc_exit. works together with the channel and with
no_patch mode.

**process marking**

marked processes get `TIF_SYSCALL_TRACEPOINT` and their syscalls
fire the tracepoint. `tp_mark_all` true (default) marks every
process, false marks only the current task. `tp_mark_cb` replaces
the built-in flag toggling with a consumer callback
`void (*)(struct task_struct *p, bool on)`, for selective marking.

`tp_tier_cb` adds a graded decision: it returns a mask of
`SC_TP_TIER_ROOT_DOMAIN`, `SC_TP_TIER_ZYGOTE`, `SC_TP_TIER_INIT`,
`SC_TP_TIER_SHELL` and `SC_TP_TIER_ALLOW_UID` bits for a task,
`tp_tier_mask` (default all of them) picks the tiers that stay
marked and every task outside of them is cleared. it runs from the
marker with the task list under rcu_read_lock and preemption
disabled, so it must not sleep. left unset the marking stays
ungraded, which is the old behavior. with `tp_mark_cb` set the tier
decision still decides and the callback performs it.

**coexistence with ftrace and perf**

the kernel marks every task itself when the sys_enter tracepoint
turns on and clears them when its last user leaves, so a second user
of the tracepoint must not meet unmarked tasks. the library follows
the transitions with kretprobes on `syscall_regfunc` and
`syscall_unregfunc` and reads the live probe array when it has to
decide. marking is graded only while the library is the sole user:
as soon as another user is present every task is marked, and the
teardown leaves the marks alone.

**int sc_tp_reg_count(void)**

number of users of the sys_enter tracepoint, from the live probe
array, falling back to the kretprobe count when the array cannot be
read. more than 1 means another user is present, which is when a
consumer should yield.

**tp_on_enter**

optional observer `void (*)(int id, struct pt_regs *regs)`. called
for every syscall of a marked process, before the redirect check.
use it to watch or adjust registers of unhooked syscalls.

**int sc_tp_register(int nr, sc_tp_hook_fn fn)**

route syscall nr to fn. -EINVAL for bad nr or NULL fn. -EEXIST when
nr is already hooked. no table write happens.

**void sc_tp_unregister(int nr)**

stop routing nr. silent for unhooked nr.

**bool sc_tp_hooked(int nr)**

true when nr has a registered hook.

**long sc_tp_orig(int nr, const struct pt_regs *regs)**

call the current table entry for nr. the standard way for a hook to
chain to the original syscall. the internal call is `__nocfi`, the
table entry typeid matches the syscall_fn_t signature. hook
signature:

```c
typedef long (*sc_tp_hook_fn)(int nr, const struct pt_regs *regs);
```

the typedef carries the KCFI typeid, so the dispatcher's indirect
call is check-compatible.

**int sc_tp_slot(void)**

the dispatcher slot number. -1 when disabled.

**slot discovery**

the dispatcher scans `sys_call_table` for a ni entry, skipping the
channel slot and every patched slot. consumer find_slot callbacks
are not consulted, they already own the channel slot selection.

**slot conflicts**

registering a hook on the dispatcher slot itself is allowed but
never fires: the sys_enter handler skips the redirect for the
dispatcher slot, so direct calls keep the ni behavior. registering
on the channel slot routes calls through the hook, then sc_tp_orig,
then sc_handler, so the channel stays reachable as long as the hook
passes through.

**teardown**

sc_exit unregisters the tracepoint, then resolves `tracepoint_srcu`
and calls `synchronize_srcu` to wait out in-flight probes before
freeing module memory. on kernels where those symbols are trimmed
by CONFIG_TRIM_UNUSED_KSYMS it falls back to `synchronize_rcu`.

## Write path

`sc_patch` writes the table through a function the consumer supplies:
`sc_cfg.patch_write`, with `sc_cfg.patch_priv` handed back to it. NULL keeps the
library default, `hk_patch_write`, which is the path `hk_init` was configured
with, so a consumer that supplies nothing gets whatever it chose for HooKern.

A consumer that wants another path, a fallback, or a test before the write
supplies its own:

```c
static int my_write(void *addr, unsigned long val, void *priv)
{
	if (hk_va_writable((unsigned long)addr))
		return hk_write_direct(addr, &val, sizeof(val));
	if (!hk_write_fixmap(addr, &val, sizeof(val)))
		return 0;
	return hk_write_kernel(addr, &val, sizeof(val));
}

struct sc_cfg cfg = { ... .patch_write = my_write };
```

The library keeps no policy of its own about this and no state beyond the two
pointers.

## Slot discovery

selection order, first hit wins:

1. `cfg->find_slot`: consumer's own method (its logic, or anything
   it wants)
2. `layout->find_slot`: layout library probe
3. library default find_slot_scan: resolve `sys_call_table`
   and `__arm64_sys_ni_syscall` (plus `.cfi_jt`), then scan
   `sys_call_table[0..511]` with the safe read and match the first
   ni entry. the internal default is a static function; the
   exported wrapper `sc_find_slot_scan` is only built under
   CONFIG_KERNSC_DISCOVER

this is a consumer choice, not a fallback chain: nothing fails
over, an unset find_slot simply means the default is used. the
default is runtime based, no compile-time assumptions.

## Dispatch contract

when dispatch runs:

- the key already matched and the caller is root (euid == 0). the
  root check is what a command outside of both tables gets, a
  command of a table runs after its own grade
- regs[0] is the key pointer, regs[1] is the command, regs[2..5]
  are the remaining syscall arguments. the positions have names:
  `SC_ARG_KEY`, `SC_ARG_CMD`, `SC_ARG_ARG0` to `SC_ARG_ARG3`, with
  `SC_ARG_NONCE` and `SC_ARG_SEQ` aliasing the first two and
  `SC_ARG_BATCH` the third, see Replay guard and Batch submission
- `priv` is the cfg.priv pointer, unchanged
- user pointers in regs must be accessed with copy_from_user /
  copy_to_user as usual

## Command table and permission grades

the built-in table is consulted first, then the consumer table
`cmds` / `ncmds`, then `dispatch`. a command in neither table keeps
the legacy path: key, then euid 0.

```c
struct sc_cmd {
	unsigned long cmd;
	const char *name;
	long (*handler)(const struct pt_regs *regs, void *priv);
	bool (*perm_check)(const struct pt_regs *regs, void *priv);
	unsigned int perm;
	unsigned char fd_arg;
	bool no_replay;
	bool read_only;
};
```

the grades, checked against the caller of the command:

- `SC_PERM_ROOT`: euid 0
- `SC_PERM_MANAGER`: `is_manager`
- `SC_PERM_ROOT_MANAGER`: either of the two
- `SC_PERM_SU`: `is_manager` or `is_allow_uid` of the real uid
- `SC_PERM_DOMAIN`: `is_domain` of the current task
- `SC_PERM_ALWAYS`: no check at all
- `SC_PERM_CUSTOM`: `perm_check` decides

a denied command returns -EPERM. `fd_arg` is the register index of
an fd that may carry the grade as a permission bit, `no_replay`
exempts the command from the replay guard, `read_only` marks the
command for the fast path. `handler` gets the untouched regs and
cfg.priv, a NULL handler returns -ENOSYS.

## Fast path

an entry marked `read_only` and `no_replay` runs on a path that
takes no lock and allocates nothing: the key was compared against
the library copy, the grade is checked, and the handler is called.
the library holds no lock of its own there, the identity callbacks
and the handler are consumer code and answer for themselves. the
criterion is the pair of marks, a read only command has no side
effect, so a repeat of it changes nothing and it needs no session,
and it has nothing to gain from the fd capability either. a
`read_only` entry without `no_replay`, or one that carries an
`fd_arg`, is refused the fast path and sc_init warns about it once,
so nothing is skipped silently.

what the fast path skips and what it keeps:

| step | slow path | fast path |
|------|-----------|-----------|
| key compare | copy of key length, no lock | same |
| grade check | yes | yes |
| fd capability | consulted when the grade failed | skipped |
| replay guard | yes when replay_enable and not no_replay | skipped |
| locks | replay and fd table spinlocks | none |
| allocation | none | none |

the contract for a consumer entry marked `read_only`: it must not
have a side effect, must not sleep and must not take a lock of its
own, otherwise the fast path is only fast on paper. the built-in
default set is `SC_CMD_HELLO`.

## Batch submission

one call carries up to `SC_BATCH_MAX` (16) commands. the caller
passes a pointer to a `struct sc_batch` in regs[SC_ARG_BATCH] (4):

```c
#define SC_BATCH_MAX 16
#define SC_BATCH_ARGS 4

struct sc_batch_entry {
	long cmd;
	unsigned long arg[SC_BATCH_ARGS];
};

struct sc_batch {
	unsigned long count;
	long ret[SC_BATCH_MAX];
	struct sc_batch_entry entry[SC_BATCH_MAX];
};
```

`count` and `entry` are read, `ret` is written per entry. the
arguments of an entry land in regs[SC_ARG_ARG0..3], which is
regs[2..5], so a handler reads a batched command exactly like a
direct one and its `fd_arg` keeps working. only those six registers
carry meaning for an entry, the rest of the register file is zeroed
rather than copied from the batch call. the descriptor is copied
in and out inside the call, no user pointer is kept past it, and the
per entry codes travel back in one copy instead of one syscall and
one return path per command.

the return value of the call is the number of entries that ran, or a
negative error for the container itself: -EINVAL when the pointer is
NULL or count is 0 or above `SC_BATCH_MAX`, -EFAULT when the
descriptor cannot be copied.

each entry runs on its own grade, an entry with a side effect keeps
its replay duty at the level of the batch request, and an entry that
is unknown falls to the legacy `dispatch` path with its root rule.
the fd form of an entry keeps working too, its fd is read from its
own argument, and only a `read_only` entry gives that up. the per
entry codes are the result of the command, or -EPERM, -EINVAL for a
nested batch, or -ENOSYS.

**execute all, not stop on error**: a failed entry does not stop the
ones behind it, and the remaining entries still run. a batch is a
polling primitive, the caller asks N questions at once and wants N
answers, stopping at the first error would throw away the answers it
already paid for. the codes tell the caller which entry failed.

**interrupt**: a pending fatal signal stops the loop before the next
entry, the entries that did not run are marked -EINTR and the call
returns the number that did run, so the partial results are not lost.
an ordinary pending signal does not stop a batch, a daemon with a
pending SIGCHLD is not refused.

**nesting**: a batch entry may not be `SC_CMD_BATCH`, it returns
-EINVAL. a nested batch multiplies the fan out and each level holds
its own descriptor on the kernel stack. a consumer handler that
calls `sc_batch_run` itself answers for the depth it allows, every
level costs about 1.1 KB of stack.

**boundaries**:
- at most 16 entries per call, 4 arguments per entry
- the descriptor lives on the kernel stack, the runner frame is
  around 1.1 KB, and no heap is touched on the call path
- entries run one after another on the calling thread, a batch never
  runs entries in parallel
- concurrent calls on the same channel are independent: the fast path
  takes no lock at all, the replay and fd tables take their own, and
  a command that keeps state has to lock it itself like before

**long sc_batch_run(const struct pt_regs *regs)**

the runner behind the built-in command, it reads the descriptor from
regs[SC_ARG_BATCH] and applies no authorization of its own. a
consumer table entry can call it to give the container a different
grade than the built-in `SC_CMD_BATCH`.

## fd scoped permissions

a permission word attached to a file authorizes a command that the
caller identity denies. the intended use is an fd created once at su
startup and dropped again when the window closes.

```c
#define SC_FD_PERM(grade) (1u << (grade))

int sc_fd_perm_grant(struct file *file, unsigned int perms,
		     unsigned long ttl_ms);
unsigned int sc_fd_perm_get(struct file *file);
void sc_fd_perm_clear(struct file *file);
```

the bits share the `sc_perm` numbering, so `SC_FD_PERM(SC_PERM_SU)`
is the su session bit. `ttl_ms` 0 grants without expiry, otherwise
the grant stops matching that many milliseconds after it was made.
16 files fit, -ENOSPC when full, -EINVAL for a NULL file or an empty
word. the library holds a reference on the file and releases it in
`sc_fd_perm_clear` and in `sc_exit`. a command authorizes through
its fd when `fd_arg` is nonzero and the fd carries
`SC_FD_PERM(perm)`, and only after the identity check failed.
`sc_sock_file` hands out the file of a socket for consumers of the
socket family.

## Replay guard

`replay_enable` adds a session check to every command of a table
whose `no_replay` is unset. a session is a nonce plus a sequence
number, carried by the syscall arguments:

```
regs[0] key   regs[1] command   regs[2] nonce   regs[3] sequence
```

a session is opened with `SC_CMD_SESSION`, which generates the nonce
and copies it to the user pointer in regs[2]. every later call has
to present that nonce and a sequence number strictly above the last
accepted one, everything else is refused with -EACCES, so a repeated
or a reordered request never reaches the handler. sessions are keyed
by thread group id, `replay_sid` replaces that key, `replay_ttl_ms`
expires a session, 16 of them fit. `sc_replay_open` and
`sc_replay_close` seed and drop one from the kernel side. a guarded
command has to keep regs[2] and regs[3] free for the guard, the
legacy `dispatch` path has no session and stays unguarded. a batch
is one request: it carries one nonce and one sequence for the whole
batch and its entries are not checked one by one, while a batched
entry marked `no_replay` behaves as it does on a direct call.

the call path copies the configured key length plus one byte instead
of the whole 64 byte buffer, and the compare runs against the copy
the library took at init, so it needs no lock.

## Built-in commands

**SC_CMD_HELLO / SC_MAGIC**

returns SC_MAGIC (0x53434831) after key and root checks. commonly
used as a channel health check from the userland side, exempt from
the replay guard.

**SC_CMD_SESSION**

opens a replay session for the caller of the command, see Replay
guard. root only, exempt from the replay guard, -ENOSYS when
`replay_enable` is unset.

**SC_CMD_BATCH**

runs the batch of regs[SC_ARG_BATCH], see Batch submission. grade
`SC_PERM_ROOT_MANAGER` for the container, every entry is still
checked on its own grade. not exempt from the replay guard: a batch
is one request and takes one nonce and sequence pair.

## Discovery API (CONFIG_KERNSC_DISCOVER)

**unsigned long sc_table_addr(void)**

address of `sys_call_table`, 0 when not resolved.

**unsigned long sc_entry(unsigned long nr)**

value of table entry nr via the safe read, 0 on failure.

**int sc_find_slot_scan(void)**

the library default discovery: full table scan for the first
ni_syscall entry. exported so consumers can call it directly.

## Patch API (CONFIG_KERNSC_PATCH)

the patch API lets consumers fix or replace any syscall slot,
beyond the channel's own slot. combined with the callbacks this
covers every syscall modification use case.

**int sc_patch(unsigned long nr, unsigned long handler, unsigned long *orig_out)**

replace any syscall slot with handler. reads and returns the
original entry in orig_out. -EINVAL for nr >= 512. -ENODATA when
the table is not resolved. -EFAULT when the original entry cannot
be read. -EEXIST when the slot is already patched. -ENOSPC when
the patch table (16 slots) is full. the channel itself is patched
this way internally. the write goes through HooKern, which parks
the other cores with stop_machine, so this and `sc_unpatch` are
process context only and a write that failed is returned instead
of being ignored.

**void sc_unpatch(unsigned long nr)**

restore the original entry. silent when nr is not patched.

## Socket protocol family (sc_sock)

a custom socket protocol family for kernel to userland event
streaming. no `/dev` node, no filesystem dependency. the family is
registered as a library component, consumers link `sc_sock.o` and
call the API directly.

**int sc_sock_init(const struct sc_sock_consumer_ops *ops)**

register the protocol and scan a free family number starting from
`AF_DECnet`. ops is optional, NULL is allowed. 0 on success,
-EADDRINUSE when no free family is found.

**void sc_sock_exit(void)**

unregister the protocol and family. module refcount is held per open
socket, so unload is blocked while sockets are alive.

**int sc_sock_family(void)**

the registered family number, -1 before init or after exit.

**int sc_sock_send_event(const void *data, size_t len)**

broadcast one event to every open sc_sock socket. -ENODEV when the
family is not registered, -EINVAL for NULL data or bad length,
-ENOMEM when skb allocation fails. `SC_SOCK_EVENT_MAX` bounds the
event size.

**int sc_sock_send_event_to(const void *data, size_t len, struct socket *sock)**

send one event to a single socket.

**void *sc_sock_priv(struct socket *sock)**

per-socket consumer private data, NULL when unset.

**int sc_sock_set_priv(struct socket *sock, void *priv)**

set per-socket consumer private data.

**struct file *sc_sock_file(struct socket *sock)**

the file behind the socket, the handle `sc_fd_perm_grant` takes.
NULL for a NULL socket.

```c
struct sc_sock_consumer_ops {
	int (*ioctl)(struct socket *sock, unsigned int cmd,
		     unsigned long arg);
	int (*sendmsg)(struct socket *sock, struct msghdr *msg, size_t len);
	int (*mmap)(struct file *file, struct socket *sock,
		    struct vm_area_struct *vma);
	int (*setsockopt)(struct socket *sock, int level, int optname,
			  sockptr_t optval, unsigned int optlen);
	int (*getsockopt)(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen);
};
```

the consumer ops are optional extension points. when NULL, ioctl,
sendmsg, mmap, setsockopt and unknown getsockopt return the default
-EOPNOTSUPP / -ENOTTY / -ENOPROTOOPT.

userspace protocol:

```c
#define SC_SOCK_PROTO 0x53
#define SC_SOCK_LEVEL 0x5343
#define SC_SOCK_OPT_HELLO 0x1000
#define SC_SOCK_OPT_FAMILY 0x1001

int fd = socket(AF_DECnet, SOCK_RAW, SC_SOCK_PROTO);
int val;
socklen_t len = sizeof(val);

getsockopt(fd, SC_SOCK_LEVEL, SC_SOCK_OPT_HELLO, &val, &len);
getsockopt(fd, SC_SOCK_LEVEL, SC_SOCK_OPT_FAMILY, &val, &len);
recv(fd, buf, sizeof(buf), 0);
```

socket creation requires `CAP_NET_BIND_SERVICE`. events are queued
per socket and read with recv. recv blocks when no event is queued
unless MSG_DONTWAIT is set.

## Build options

```
KDIR=...            kernel build dir, required
KERNSC_MINIMAL=1    minimal build: custom syscall core only
                    (sc_init + handler + default discovery),
                    patch, discovery, tracepoint dispatcher and
                    sc_sock are compiled out
```
