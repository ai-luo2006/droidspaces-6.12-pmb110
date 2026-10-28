> English version of [PIDS-ENFORCEMENT.md](../PIDS-ENFORCEMENT.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# pids Controller Functional Layer (Plan B)

> Goal: make Droidspaces' `--pids-limit` **actually constrain the container**, rather than merely ticking a box in the checklist.
>
> Current state: the target kernel `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` was built without
> `CONFIG_CGROUP_PIDS`, so there is no pids subsystem in the kernel. An LKM cannot conjure a cgroup
> subsystem out of thin air, but it can **emulate** one: a readable and writable `pids.max` file plus
> a code path that rejects `fork()`.

Code location: `droid_lkm/src/pidsmod/`

| File | Responsibility |
| --- | --- |
| `pids_main.c` | Display layer: makes the kernel print the name `pids` |
| `pids_ctl.c` | Functional layer: `pids.max` file + fork interception + `subtree_control` translation |

---

## 1. Two layers, each solving half of the problem

The failure on the Droidspaces side happens in two steps: first it **cannot find** the pids controller
(it prints `pids.max is not available` and the check item is marked ✗), and only then does the
**limit fail to take effect**.

### Display layer (A)

Hook two functions whose only job is to "print":

- `cgroup_controllers_show` → `pids` appears in `cgroup.controllers`
- `proc_cgroupstats_show` → `pids 0 1 1` appears in `/proc/cgroups`

Droidspaces only checks the controller **name**, so this step alone is enough to turn the
`Process limit support` check item green. It does not change any behavior; it is pure display.

### Functional layer (B)

| No. | Problem | Solution |
| --- | --- | --- |
| B1 | No `pids.max` to write | Attach one to the memory controller |
| B2 | Writing it does not constrain anything | kprobe intercepts fork and returns `-EAGAIN` |
| B3 | DS's `+pids` makes the whole `subtree_control` write fail | Translate `+pids` into `+memory` |

---

## 2. B1: how `pids.max` is created

The kernel has no pids subsystem, but `cgroup_add_dfl_cftypes()` allows appending control files to an
**already existing** subsystem. memory is a controller that Droidspaces always enables, hence:

```c
ret = cgroup_add_dfl_cftypes(memory_ss, pids_max_files);
```

All three files in `pids_max_files` carry `CFTYPE_NO_PREFIX | CFTYPE_NOT_ON_ROOT`, so the names are
taken literally and do not turn into `memory.pids.max`:

| File | Permission | Purpose |
| --- | --- | --- |
| `pids.max` | rw | Receives the limit written by Droidspaces, stored into the per-cgroup `limit` |
| `pids.current` | ro | Hierarchically accumulated current count (including descendant cgroups) |
| `pids.stats` | ro | Probe self-check counters, see §6 |

Per-cgroup state lives in an xarray, keyed by `struct cgroup *`:

```c
struct pids_priv {
	atomic64_t counter;	/* this cgroup + all descendants */
	atomic64_t limit;	/* 0 = not enabled; S64_MAX = "max" */
};
```

When the memory controller is disabled, `cgroup_add_dfl_cftypes()` returns 0 and adds nothing —
a graceful degradation.

---

## 3. B2: fork interception

### 3.1 Accounting points

| kprobe | Action |
| --- | --- |
| `cgroup_can_fork` | Over the limit → write `-EAGAIN` into x0 and return directly; the function body never runs |
| `cgroup_post_fork` | Accounting `+1` |
| `cgroup_exit` | Accounting `-1` |
| `cgroup_rmdir` | Clears that cgroup's state, so a stale limit is not reused after pointer reuse |

Both `pids_charge()` and `pids_check()` walk toward the root along `cgroup->self.parent`, so a limit
set on the container directory constrains the entire subtree — that is exactly the semantics of the
real pids controller (container processes actually live in child cgroups such as `ds-enter` /
`init.scope` / `system.slice`).

### 3.2 How the rejection is delivered

A kprobe is for "observation", not "replacement", so the function body cannot be modified. The
approach is to write the return value into `x0` and then return through the link register, so that
the probed function does not execute at all for this call:

```c
atomic_inc(&ctl_n_refused);
regs->regs[0] = (unsigned long)(-EAGAIN);
hk_regs_return(regs);
return 1;
```

After `copy_process()` receives `-EAGAIN`, it unwinds along the normal failure path, and the `fork()`
system call returns `EAGAIN` to userspace — identical to what the real pids controller does when the
limit is exceeded.

### 3.3 Three pitfalls hit at the accounting points

This part consumed the most time in the project; it is written down so nobody repeats them:

1. **Accounting inside `cgroup_can_fork` → the counter only ever goes up.**
   A failed fork goes through `cgroup_cancel_fork()`, which walks the kernel's own subsystem list and
   never reaches the module. So the charge had to be moved to `cgroup_post_fork`, where creation has
   definitely succeeded.

2. **Reading `child->cgroups` inside `cgroup_post_fork` → always 0.**
   The pre-handler fires at function entry, while `rcu_assign_pointer(child->cgroups, cset)` lives in
   the function body and only runs a few lines later; at that point the field still holds the old
   value from when the child was born. The fix was to read `kargs->cset` instead — it is already
   filled in earlier, in `cgroup_can_fork()` → `cgroup_css_set_fork()`.

3. **Counting only the current level → the container shows `current=1`.**
   Only after switching to accumulation along the ancestor chain do we get real pids semantics.

### 3.4 Why kprobe and not an inline hook

- The inline hook trampoline for `cgroup_procs_write` contains `BRK #4` (`d4200080`). As soon as
  ksud writes `cgroup.procs`, it lands on it — **the whole device lost power 22 ms after loading**.
  That hook has been removed entirely, and a load-time trampoline trap self-check was added.
- For every target in this file that is "called directly", an inline hook can be installed but
  **never fires even once** (`cgroup.subtree_control` still rejects `+pids`, and `pids.current` is
  still 0 after several hundred forks).
- `register_kprobe()` places the breakpoint under `text_mutex` + `cpus_read_lock`; it does not depend
  on relocating the target function's entry, and is a mechanism maintained by the kernel itself.

### 3.5 Struct layout self-check

`struct css_set` starts with `subsys[CGROUP_SUBSYS_COUNT]`, so the offset of `dfl_cgrp` depends on how
many controllers the running kernel was built with. At runtime the module counts the actual number
from `cgroup_subsys[]`, derives the offset, and then performs a **double check**:

```
init_css_set.dfl_cgrp == &cgrp_dfl_root.cgrp
init_task.cgroups     == &init_css_set
```

If either check fails → `ctl_layout_ok = false` → **not a single fork / exit / rmdir probe is
installed**, leaving only the display layer. The cost of a wrong offset is missing functionality,
never reading random memory.

---

## 4. B3: `subtree_control` translation

To obtain `pids.max`, Droidspaces writes `+memory +cpu +pids` (or only `+pids`) into the container
cgroup's `cgroup.subtree_control`. The kernel does not recognize pids, so the entire write fails and
returns `-EINVAL`, **dragging memory down with it** — this is exactly why the container directory
never had memory, and therefore never had `pids.max`.

The kprobe on `cgroup_subtree_control_write` acts before the kernel parses the buffer:

1. Blank out the two tokens the kernel does not have, `pids` and `cpu`, replacing them with spaces
   (the `+`/`-` prefix is blanked out together with the token, and a token boundary is required so
   that `cpuset` is not shaved down to `set`);
2. If something remains after blanking (e.g. `+memory`) → hand it to the kernel to parse as usual;
3. If nothing remains after blanking and the original request contained `+pids` → replace the buffer
   with a per-CPU `"+memory"` and rewrite `regs[1]` / `regs[2]`. This is needed because the buffer
   handed in by kernfs is allocated according to the length the user wrote, and `"+pids"` is only
   6 bytes — too small to hold `"+memory"`.

**Replacement happens only once per cgroup**: once memory is enabled it is sticky, and enabling it
repeatedly makes Droidspaces receive `Bad address` (EFAULT).

---

## 5. The full chain

```
DS creates a container
  └─ writes cgroup.subtree_control "+memory +cpu +pids"
       └─ [kprobe] translated into "+memory"          ← B3
            └─ container cgroup gets the memory css
                 └─ the pids.max file appears          ← B1

DS reads cgroup.controllers / /proc/cgroups → sees "pids"   ← display layer

DS writes pids.max = 256                                     ← B1
  └─ stored into the xarray

Container forks
  └─ cgroup_can_fork → [kprobe] over limit? → -EAGAIN         ← B2
  └─ cgroup_post_fork → [kprobe] counter +1
  └─ cgroup_exit      → [kprobe] counter -1
```

---

## 6. `pids.stats`: self-check counters

```
subtree=3644 strip=48 rewrite=36 fork=10530 charged=10530 refused=629 rmdir=378 layout=1
```

| Field | Meaning | Criterion |
| --- | --- | --- |
| `subtree` | Number of `subtree_control` writes reaching the probe | > 0 means the probe is running |
| `strip` | Number of illegal tokens blanked out | — |
| `rewrite` | Number of times an entire request was replaced with `+memory` | — |
| `fork` | Number of `cgroup_can_fork` triggers (**system-wide**, not just the container) | Steady growth = the probe is alive |
| `charged` | Of those, the number where the target cgroup resolved successfully | Close to `fork` |
| `refused` | **Number of forks intercepted and returned with `-EAGAIN`** | **> 0 means the functional layer is working** |
| `rmdir` | Number of cgroup directory reclaims | — |
| `layout` | Layout self-check result | Must be 1 |

The point of this file is to distinguish two kinds of failure: **the probe is not installed** vs
**the probe is installed but lets everything through**. If `fork` does not move, the probe was placed
at an address the kernel never reaches (while the logs look perfectly normal).

---

## 7. Real-device verification record

Device: OnePlus Ace 6 Ultimate Edition `PMB110`, `17.0.0.101(CNO1C10P02)`,
kernel `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`, 2026-10-28.

### 7.1 On the Droidspaces side

- No longer prints `pids.max is not available`
- `pids.max` / `pids.current` / `pids.stats` appear in the container directory
- The `Process limit support` check item passes

### 7.2 Manual cgroup controlled experiment

```sh
su -c 'CG=/sys/fs/cgroup/t2; rmdir $CG 2>/dev/null; mkdir -p $CG; echo 5 > $CG/pids.max; echo $$ > $CG/cgroup.procs; read S < $CG/pids.stats; echo "start   $S"; i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); read C < $CG/pids.current; read S < $CG/pids.stats; echo "第${i}次 current=$C  $S"; done; rmdir $CG 2>/dev/null'
```

Measured output:

```
start   subtree=3644 strip=48 rewrite=36 fork=10458 charged=10458 refused=624 rmdir=378 layout=1
第1次 current=1  ... fork=10460 ... refused=624
第2次 current=2  ... fork=10462 ... refused=624
第3次 current=3  ... fork=10464 ... refused=624
第4次 current=4  ... fork=10466 ... refused=624
第5次 current=1  ... fork=10524 ... refused=629
第6次 current=2  ... fork=10526 ... refused=629
第7次 current=3  ... fork=10528 ... refused=629
第8次 current=4  ... fork=10530 ... refused=629
```

How to read it:

- `current` climbs to 4 and cannot go any higher; `refused` goes **+5** between the 4th and 5th run.
- There is a pause of about 6 seconds between the 4th and 5th run, during which `fork` increases by
  58 (system-wide). This is not a hang: when `fork()` returns `EAGAIN`, `/system/bin/sh`
  **waits and retries**, and only succeeds once the first batch of `sleep 6` expires and the quota is
  released — so `current` falls back to 1 and climbs again.
- The `refused` baseline of 624 comes from **a real container that is running** (limit=80), showing
  that the functional layer keeps intercepting for DS containers as well.

### 7.3 User-side controlled experiment

| `--pids-limit` | Symptom |
| --- | --- |
| 32 | Container stop times out and is eventually SIGKILLed (quota too small; the container cannot even start itself) |
| 80 | `docker stop` exits cleanly |

The same container with only the limit changed: once it cannot be stopped, once it stops normally —
this shows the interception really lands on the fork path.

### 7.4 Stability

No `Unable to handle kernel paging request` / `panic` / `BUG:` / `CFI failure` in `dmesg`.
All five probes installed successfully:

```
[droid_lkm_pids_ctl] layout verified: controllers=8 (built against 8)
[droid_lkm_pids_ctl] probed cgroup_subtree_control_write at 0xffffffe20458db44
[droid_lkm_pids_ctl] probed cgroup_can_fork at 0xffffffe2043a0a50
[droid_lkm_pids_ctl] probed cgroup_post_fork at 0xffffffe2043cfcd4
[droid_lkm_pids_ctl] probed cgroup_exit at 0xffffffe2044e7068
[droid_lkm_pids_ctl] probed cgroup_rmdir at 0xffffffe2047d1f2c
[droid_lkm_pids_ctl] pids limit enforcement active (emulated pids controller)
```

---

## 8. Known deviations and limitations

1. **Processes migrated in are not counted immediately.**
   The counter only counts processes that were forked inside the cgroup. The one migrated in by
   writing `cgroup.procs` is not counted (the real controller would count it immediately). So
   `pids.max=5` can actually hold 5 processes while `pids.current` only reaches 4. In real scenarios
   with `--pids-limit=256`, this difference of 1 has no impact.

2. **`--pids-limit` is recommended to be ≥ 256.**
   An otherwise idle container itself needs about 68 processes. Setting 32 starves the container
   (manifested as a stop timeout followed by SIGKILL).

3. **Repeated `enable` results in EFAULT.** B3 already deduplicates per cgroup
   (`CTL_REWRITTEN_MAX = 8` slots). Once the slots are used up no further replacement happens; if DS
   then re-sends `+pids`, it gets `Bad address` — an expected degradation.

4. **`pids.current` is a hierarchically accumulated value** (this cgroup + all descendants), which
   matches the real semantics; it is not "the number of processes directly attached to this
   directory".

5. **Verified only on this device / kernel.** Changing the kernel requires a rebuild, and if the
   layout self-check fails, the functional layer automatically degrades to a pure display layer (it
   will not crash, it just stops limiting).

6. **Every probe is best effort**: if it cannot be installed it is skipped, the kernel is left
   untouched, functionality is missing but it never crashes.
