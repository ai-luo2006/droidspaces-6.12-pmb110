> English version of [PRINCIPLE.md](../PRINCIPLE.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# Implementation Principle (Open-Source Notes)

This document explains **exactly what each module does**, so that reviewers can judge the risk and
portability.

---

## 0. What the target kernel is missing

The GKI 6.12 kernel used by the OnePlus Ace 6 Ultimate Edition is trimmed for Android:

| Config | Status | Impact |
| --- | --- | --- |
| `CONFIG_PID_NS` | n | Containers cannot see an independent PID space |
| `CONFIG_IPC_NS` | n | System V IPC / POSIX mqueue cannot be isolated |
| `CONFIG_SYSVIPC` | n | `ipcs` unavailable |
| `CONFIG_POSIX_MQUEUE` | n | mqueue unavailable |
| `CONFIG_USER_NS` | n | Cannot map uid/gid inside the container. The LKM can only translate ids at syscall boundaries — **this is not isolation**; container root is host root (see 1.3 and `CHANGELOG.md`) |
| `CONFIG_DEVTMPFS` | n | The container cannot get real device nodes under `/dev`; "hardware access" cannot be turned on |
| `CONFIG_MACVLAN` | n | The container cannot get an independent LAN IP |
| `CONFIG_IP_NF_NAT` / ip6 nat | partially missing | IPv6 for `--net=nat` does not work |
| `CONFIG_CGROUP_PIDS` | n | `--pids-limit` not available natively; this repo emulates it with an LKM (see PIDS-ENFORCEMENT.md) |

Without flashing a kernel, an LKM is the only way to backfill these at runtime.

---

## 1. The three main modules

### 1.1 `droid_lkm.ko` — namespace / IPC

Core techniques:

1. **Type-info BTF**: when the kernel carries BTF, structure layouts can be rebuilt at runtime from
   the BTF descriptions, with no need to hardcode offsets (this is the key to portability across
   kernel versions).
   Source: `droid_lkm/src/main/core/ds_ti.c`.
2. **Symbol resolution**: `droid_lkm/src/main/core/ds_ksym.c` uses KallRecon to recover the kallsyms
   table from the kernel image and looks up addresses by name, bypassing
   `CONFIG_TRIM_UNUSED_KSYMS=y` and `kptr_restrict=2`.
3. **System call rewriting**: KernCall rewrites the syscall table, replacing entries such as
   `unshare`, `clone` and `fork` with its own implementations, which create/switch namespaces.
   Source: `droid_lkm/src/main/ipcns/`.
4. **IPC virtualization**: maintains an independent view of System V IPC and mqueue for each
   namespace (`droid_lkm/src/main/ipcns/sysv/`, `mqueue/`).

Module parameters (the ones actually used):

```
gate=0            main feature switch
mqueue=1          enable POSIX mqueue
nspid=1           enable PID namespace
nspid_append=1    pid append mode
skip_sysvipc=0    enable System V IPC
no_fake_ns=0      do not use a fake namespace
skip_do_exit=0    enable do_exit path handling
```

### 1.2 `droid_lkm_vendor.ko` — vendor compatibility

Vendor modifications in OPPO/OnePlus kernels make standard hooks ineffective; this module does two
things:

- **ghost task**: `find_task_by_vpid` fails to find certain pids on vendor kernels, so a
  self-maintained table is used as a fallback (`droid_lkm/src/compat/vendor/ghost.c`).
- **ddl history table**: the vendor's ddl record structure changes between versions, so the layout is
  chosen by runtime probing (`droid_lkm/src/compat/vendor/ddl_hit.c`).

Parameters:

```
ghost=1             enable ghost task fallback
ghost_match=oplus_  pid prefix matching rule (only pids with the oplus prefix are added to the table)
pid_max_cap=0       no cap on the pid upper bound
```

### 1.3 `droid_lkm_misc.ko` — userns view (inactive on this device) + devtmpfs

**user namespace** (`droid_lkm/src/misc/userns/`):

> **Bottom line**: this kernel is built without `CONFIG_USER_NS`. The code is kept, but it
> provides **no isolation whatsoever**. Evidence chain in `CHANGELOG.md`.

What `CONFIG_USER_NS=n` actually means:

- `kernel/user_namespace.c` is not compiled at all — `create_user_ns()` does not exist
- `get_user_ns` / `put_user_ns` / `make_kuid` / `from_kuid` are `static inline` identities,
  inlined at every call site, with no function address to hook
- `ksys_unshare()` returns `-EINVAL` for `CLONE_NEWUSER`
- `/proc/<pid>/ns/user` does not exist; `/proc/<pid>/uid_map` is always `0 0 4294967295`

Two things remain, neither of which is isolation:

1. **unshare fake-success** — `CLONE_NEWUSER` is stripped from the flag word and the rest is
   passed to the kernel. Droidspaces then reports "sandbox available", while the container
   still runs in the host's init user namespace.
2. **id translation view** — `getuid/geteuid/getgid/getegid` return values are rewritten on the
   way out; `setuid/setresuid/setreuid/setfsuid/chown/setgroups` map container ids to host ids
   on the way in; `vfs_getattr` rewrites `struct kstat` uid/gid. These hooks are armed when
   `real_userns=1`, but since no process is ever in a user namespace,
   `droid_lkm_userns_of()` finds nothing and every hook degrades to a pass-through.

**Abandoned paths — do not revisit** (removed in v1.0.19):

- fabricating a `struct user_namespace` with `kmalloc` and attaching it to a cred
- swapping cred `user_ns` on `clone` / `fork`, or hijacking namespaces via
  `kernel_clone` / `do_execve`
- the `userns_adopt=1` parameter and the `$MODDIR/userns_adopt` flag file

The loop in `cap_capable()` that walks `cred->user_ns->parent` is **not** wrapped in
`#ifdef CONFIG_USER_NS`; with a fabricated structure it dereferences a wild `->parent`
pointer and panics the device. That was the v1.0.18 reboot.

**Legacy cost**: if a relabel was ever performed, rootfs ownership is now pinned at `base`
(200000 by default) and must be restored with `sh userns.sh restore all`, otherwise the
container cannot read its own files (`su: cannot set groups`). `relabel` is now gated
behind an explicit `--force`.

**devtmpfs** (`droid_lkm/src/misc/devtmpfs/devtmpfs.c`):

The kernel has `CONFIG_DEVTMPFS=n`, but `devtmpfs` must appear in `/proc/filesystems` for Droidspaces
to consider it supported.

Approach: build a filesystem named `"devtmpfs"` on top of `shmem` and register it with the kernel
(`register_filesystem`). **Registration only** — no background thread is started, no path is hooked,
and dentry/inode are never touched. Mounting and node population are entirely done by Droidspaces
itself.

> Early versions called `dget(mnt_root)` after registration to obtain the superblock's root dentry;
> at that point in the sequence it may be a null pointer → panic. Now it is never touched.

---

## 2. `macvlan.ko`

### 2.1 Why the in-kernel macvlan cannot be built directly

The target kernel has `CONFIG_MACVLAN=n`, yet `if_macvlan.h` exists in the source tree. Taking the
kernel's `drivers/net/macvlan.c` and building it as an out-of-tree module runs into three things:

1. **Unexported symbols**: the following 7 functions exist in the kernel but have no export entry
   (`CONFIG_TRIM_UNUSED_KSYMS=y`), so linking reports `Unknown symbol` directly:

   ```
   dev_forward_skb        __dev_forward_skb     dev_mc_add_excl
   ip_check_defrag        ndo_dflt_fdb_dump     eth_header_cache
   eth_header_cache_update
   ```

2. **The BUG() stub in `if_macvlan.h`**: when `CONFIG_MACVLAN` is not defined, the static inline
   function at `linux/if_macvlan.h:82` calls `BUG()`.
   → `-DCONFIG_MACVLAN=1` must be added.

3. **KCFI**: the signatures must match those used when the kernel was compiled; using the wrong
   clang shuts the device down immediately.

### 2.2 The shim approach

`net_macvlan/macvlan_shim.h` uses macros to rename the 7 symbols above:

```c
#define dev_forward_skb         droid_mv_dev_forward_skb
#define __dev_forward_skb       droid_mv___dev_forward_skb
#define dev_mc_add_excl         droid_mv_dev_mc_add_excl
#define ip_check_defrag         droid_mv_ip_check_defrag
#define ndo_dflt_fdb_dump       droid_mv_ndo_dflt_fdb_dump
#define eth_header_cache        droid_mv_eth_header_cache
#define eth_header_cache_update droid_mv_eth_header_cache_update
```

Because `-include` forces it ahead of all includes in `macvlan_core.c`, **the extern declarations in
the kernel headers are renamed as well**; the prototypes are validated by the compiler, so the
signatures cannot be wrong.

Inside `macvlan_shim.c`:

- each `droid_mv_*` is an **address slot** (`void *`);
- at module init, **KallRecon** looks up the real address by name from the kernel kallsyms table and
  fills the slot;
- the real implementation bodies (the original functions in `macvlan_core.c`, after renaming) call
  the genuine kernel functions through the slots.

This way it **does not depend on the export table, does not depend on `kptr_restrict`, and is
unaffected by KASLR** (KASLR addresses change on every boot, so resolution must happen at runtime and
cannot be hardcoded at compile time).

### 2.3 Data-plane limitation

The WiFi driver on this device accepts only one MAC and directly drops the frames sent by macvlan
sub-interfaces: measurements show the `TX` counter rising, `RX` permanently 0, and ARP forever
`FAILED`. This is a **driver limitation, not a module bug**. It works with a USB wired NIC.
host network mode does not need macvlan.

---

## 3. `ip6table_nat.ko`

A structurally identical approach, just with more symbols (9):

```
ip6t_alloc_initial_table   ip6t_register_table        ip6t_unregister_table_exit
ip6t_do_table              xt_find_table              xt_register_template
xt_unregister_template      nf_nat_ipv6_register_fn    nf_nat_ipv6_unregister_fn
```

The shim lives in `net_ip6nat/ip6t_nat_shim.{c,h}`.

> Pitfall: the 4th argument of `ip6t_register_table()` must be `const struct nf_hook_ops *` on 6.12;
> getting the type wrong still compiles but behaves incorrectly.

Result: `ip6tables -t nat -L` lists correctly, and `MASQUERADE` rules can be added and removed.

---

## 4. Loading and guards (`dsm/post-fs-data.sh`)

Load order matters:

```
droid_lkm  →  droid_lkm_vendor  →  droid_lkm_misc  →  macvlan  →  ip6table_nat
```

Guard chain:

1. **Auto-start is disabled automatically on first install** (writes `no_boot`), guaranteeing that
   "installing it will not brick the device".
2. **Bootloop guard**: writes `insmod.inflight` before insmod and deletes it on success. If that file
   is still present at the next boot → it crashed during insmod → write `blocked` and skip.
3. **Survival guard**: writes `boot_pending` after insmod succeeds; `service.sh` waits 120 seconds
   and only writes `.boot_ok` once the system is confirmed alive. At the next boot, if `boot_pending`
   exists but `.boot_ok` does not → it is judged as "crashed after loading" → write `blocked`.
4. **Device model + kernel double lock**: the kernel must be `6.12.69-*` and the device model must
   contain `PMB110`.

Loading of the network modules has **two levels of fallback**:

- Preferred: the module resolves the addresses itself (KallRecon); `insmod macvlan.ko` is enough.
- Fallback: if self-resolution fails, the script temporarily sets `kptr_restrict` to 0, reads the
  7/9 symbol addresses from `/proc/kallsyms`, and passes them in as `addr_<sym>=0x...`.

---

## 5. WebUI (`dsm/webroot/index.html`)

A pure front-end single file; it executes commands through the interface provided by KernelSU:

```js
window.ksu.exec(cmd, "{}", callbackName)
```

Three pages: Status / Logs / Diagnostics. The status page shows the load state and an individual
toggle for each module, and supports load / unload / auto-start on boot / delayed load / survival
window / unblock.

---

## 6. Risk statement

- These modules modify the kernel's namespace and IPC views, which is **a highly unconventional
  operation**. A kernel crash can reboot the phone or cause data loss.
- This project has **only been verified on the single PMB110 / 6.12.69 combination**.
- All modules have guards; **not auto-loading by default** is a deliberate design choice — please
  verify manually first.
- Do not enable auto-start on boot on your primary device unless you truly need container
  functionality and can accept the risk.
