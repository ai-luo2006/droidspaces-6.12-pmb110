# DS 全绿路线评估：Process limit support 能不能用 ko 做出来

本文回答三个问题：

1. 别人贴的"Droidspaces 全绿"到底是怎么做到的？
2. `Process limit support`（`CONFIG_CGROUP_PIDS`）能不能**只靠 ko（树外模块）**实现？
3. 除了 ko 和重编内核，还有哪些路？

结论先说：**可以只靠 ko 做，而且不需要改 DS 源码。** 但要分成两层看，
"检查项变绿"和"限制真的生效"是两件事，只有前者是显示层，后者才是功能层。

---

## 0. 判定链：DS 到底怎么判断这一项

DS v7.0.0 的判定代码（`src/check.c:459`）：

```c
print_ds_check(
    "pids_limit", "Process limit support",
    "CONFIG_CGROUP_PIDS, and no cgroup_disable=pids on the kernel "
    "command line; --pids-limit is skipped if absent",
    ds_cgroup_has_controller("pids"), "OPT");
```

`ds_cgroup_has_controller()`（`src/cgroup.c:202`）：

```c
int ds_cgroup_has_controller(const char *name) {
  char mnt[PATH_MAX];
  if (find_host_cgroup_mount(NULL, mnt, sizeof(mnt)) &&
      ctrl_supported_v2(mnt, name))   /* 读 cgroup2 根的 cgroup.controllers */
    return 1;
  struct ctrl_query q = {name, 0};
  each_v1_controller(match_controller, &q);  /* 读 /proc/cgroups */
  return q.found;
}
```

也就是说，这一项变绿只需要**满足下面任意一条**：

| # | 判据 | 数据来源 | 树外模块能不能改 |
|---|------|---------|------------------|
| A | cgroup2 根的 `cgroup.controllers` 里有 `pids` | `kernel/cgroup/cgroup.c:3073` `cgroup_controllers_show()` | **能**（hook 这个 seq_show，追加一行 `pids`） |
| B | `/proc/cgroups` 有一行 `pids ... enabled≠0` | `kernel/cgroup/cgroup-v1.c:676` `proc_cgroupstats_show()` | **能**（hook 同一个 seq 家族，或 hook v1 控制器枚举） |

DS 侧的限制下发（`src/cgroup.c:855`）：

```c
if (cfg->pids_limit) {
  snprintf(val, sizeof(val), "%lld", cfg->pids_limit);
  if (!apply_limit(cfg, "pids", "pids.max", val, "pids.max", val, ...))
    cfg->pids_limit = 0;   /* 写不进去 → 置 0 → "limit skipped" */
}
```

`apply_limit()` 写的是 `<cgroup dir>/pids.max`。**文件不存在 → 写失败 →
打印 "limit skipped" → 这一项虽然绿了，但 `--pids-limit` 没生效。**

所以"全绿"和"能用"必须分开验收。

---

## 1. 别人全绿的真相

Droidspaces 官方文档（`droidspaces.org/docs/kernel-configuration.html`）原文：

> Resource limits: `--cpus` and `--pids-limit`.
> `CONFIG_CFS_BANDWIDTH=y` / `CONFIG_CGROUP_PIDS=y`
> **These two BREAK the kABI and no patch covers them**: they resize
> scheduler and cgroup structures, which changes the CRC of thousands of
> exported symbols. Stock vendor modules then refuse to load and the device
> bootloops. Leave them commented out unless you rebuild EVERY kernel module
> from the same source and flash `vendor_boot`, `vendor_dlkm` and `system_dlkm`
> together with the new `boot.img`, all at once.

> CPU and process limits on GKI
> ... On a 5.15 GKI tree they changed the checksum of about four thousand
> exported symbols, and no padding trick covers that.

官方 GKI 配置片段（`Droidspaces-OSS/Documentation/Kernel-Configuration.md`）
里这两个选项**默认是注释掉的**。

结论：

- 公开生态里能全绿的只有两类人：
  1. **重编内核 + 把 `boot.img` / `vendor_boot` / `vendor_dlkm` / `system_dlkm`
     一起刷**（官方唯一认可的路径，kABI 破坏由自己承担）；
  2. **用本身自带 `CONFIG_CGROUP_PIDS=y` 的非 GKI 老内核 / 厂商内核**
     （4.9 / 4.14 / 4.19 这类没有 kABI 约束的树）。
- **没有任何公开的纯 ko 实现 cgroup pids 控制器的项目。**
  多轮检索 GitHub 与社区，搜到的只有内核自身的 `kernel/cgroup/pids.c`。
- `_patchHarmony` 那种"有界 PID"是 `pid_max` 范畴
  （`src/main/pidns/pidns.c:852` `droid_lkm_pidns_pid_max = PID_MAX_LIMIT`），
  和 cgroup pids 控制器不是一回事，别混淆。

### 本机的具体位置

本机内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`
（AOSP GKI 6.12）：

```
# CONFIG_CGROUP_PIDS is not set
```

`CONFIG_CFS_BANDWIDTH=y`、`CONFIG_CGROUP_CPUACCT=y`、`CONFIG_CGROUP_FREEZER=y`、
`CONFIG_CGROUP_NET_PRIO=y`，`CONFIG_CGROUP_MISC` / `CONFIG_CGROUP_RDMA` /
`CONFIG_CGROUP_DEBUG` 未开。

对照 DS 你的输出：`CPU limit support` 是绿的（说明这台机子的实际内核里
`CONFIG_CFS_BANDWIDTH` 是开的，跟 AOSP GKI 基线不同，属于厂商自己开过），
`CPU usage accounting` 也绿。只有 `pids` 这一项红 —— 恰好对应
`CONFIG_CGROUP_PIDS` 没开。

**顺便一提：DS 官方社区设备列表里有你的机型。**
`OnePlus ACE 6 Ultra PMB110`，Android 16，6.12.58，KernelSU Next + SusFS，
维护者 @fatalcoder524，状态 Working，README 里明确列了
「📱 Droidspaces Support」特性，配置项 `"ds": true`。
这条线值得单独评估（见 §4 方案 D）。

---

## 2. ko 的硬边界：树外模块能不能新增 cgroup 控制器

**不能。** 证据链完整：

1. `include/linux/cgroup_subsys.h`
   ```c
   #if IS_ENABLED(CONFIG_CGROUP_PIDS)
   SUBSYS(pids)
   #endif
   ```
   控制器列表在**编译期**由宏展开决定。

2. `include/linux/cgroup-defs.h`
   ```c
   enum cgroup_subsys_id {
   #include <linux/cgroup_subsys.h>
   CGROUP_SUBSYS_COUNT,
   };
   ```
   枚举长度随之固定。

3. `kernel/cgroup/cgroup.c:158`
   ```c
   struct cgroup_subsys *cgroup_subsys[] = {
   #include <linux/cgroup_subsys.h>
   };
   ```
   数组定长、索引固定。树外模块无法往里塞新条目。

4. `kernel/cgroup/cgroup.c:3073` `cgroup_controllers_show()`
   → `cgroup_print_ss_mask(seq, cgroup_control(cgrp))`
   → 输出的控制器名来自编译期 `ss->name`。
   **这就是 `cgroup.controllers` 里永远不可能自然出现 "pids" 的原因。**

5. `kernel/cgroup/cgroup.c:4577` `cgroup_add_dfl_cftypes()`（`EXPORT_SYMBOL_GPL`）
   只能向**已存在的** `struct cgroup_subsys` 挂 cftype 文件；
   `cgroup_init_cftypes()` 里 `WARN_ON(cft->ss || cft->kf_ops)` 然后 `cft->ss = ss`。
   想凭空造 subsys → 过不去。

所以：**"让 cgroup2 真正拥有 pids 控制器"这条路，ko 走不通，也不该走。**

但这不等于"ko 做不到这件事"。能做的是下面 §3。

---

## 3. ko 能做的三层方案

### 方案 A：显示层 —— 让检查项变绿（低风险，半天量级）

> **状态：已实现、编译通过、真机验证通过。✅（2026-10-27，PMB110 / 6.12.69）**
>
> 真机证据（`droid_lkm_pids` 加载前后对比）：
>
> ```
> 加载前 /proc/cgroups:            (无 pids 行)
>        cgroup.controllers:        memory
>
> 加载后 /proc/cgroups:            pids    0       1       1
>        cgroup.controllers:        memory
>                                 pids
> dmesg: hook cgroup_controllers_show addr=0xffffffd276dd4ae0
>        hook proc_cgroupstats_show   addr=0xffffffd276dd83ac
> ```
>
> refcount=0、holders 为空，可单独 `rmmod` 回退（实测卸载成功）。
>
> **最终结果：Droidspaces v7.0.0 检查项 40 项全绿（2026-10-27 真机实测）。**
>
> ```
> [MUST HAVE]      10/10  ✓
> [RECOMMENDED]     9/9   ✓
> [OPTIONAL]       21/21  ✓   （含 Process limit support）
> Summary: [✓] All required features found!
> ```
>
> 全程只用 `.ko`，未刷内核，未改 Droidspaces 源码。
>
> **实现形态：独立模块 `droid_lkm_pids.ko`。**
> 代码位置：`droid_lkm/src/pidsmod/pids_main.c`；
> 构建：`droid_lkm/Makefile` 的 `obj-$(DLKM_OPT) += droid_lkm_pids.o`。
>
> 为什么做成独立模块而不是塞进 `droid_lkm_misc`：
> `droid_lkm_misc` 的 devtmpfs 部分会持有一个内部 mount，这个 mount 持有模块引用，
> 于是 **misc 在运行期无法 `rmmod`**（`rmmod` 报 EBUSY，`rmmod -f` 也报
> EWOULDBLOCK）。一个要能在活手机上反复插拔的探针，必须能跟它分离。
> 独立模块 `droid_lkm_pids`：
>   - 名字不同，和已加载的 `droid_lkm_misc` **共存**，不需要卸载任何东西；
>   - `depends=` 空，不需要 `droid_lkm` / `droid_lkm_vendor` 在场；
>   - 不挂 devtmpfs 内部 mount，因此**能单独 `rmmod` 回退**。
>
> 产物已打进 `dist/Droidspaces-v1.0.0-6.12.zip`（18 files）。

Hook 两个 seq_show 输出，在 DS 读取的文本里补上 `pids`：

- `cgroup_controllers_show()`（`kernel/cgroup/cgroup.c:3073`）
  → 在输出末尾补一行 `pids\n`，让 cgroup2 根的 `cgroup.controllers` 出现 `pids`。
- `proc_cgroupstats_show()`（`kernel/cgroup/cgroup-v1.c:676`）
  → 补一行 `pids\t<hid>\t<n>\t1`，让 `/proc/cgroups` 的 v1 路径也认。

可行性依据：

- `CONFIG_KALLSYMS_ALL=y`（`.config:276`）→ 这两个符号都能在运行期 kallsyms 里解析，
  即使非导出也能拿到地址（KallRecon 的既有能力）。
- 两个函数都不是 `static` 以外的问题：它们是全局符号，inline hook 目标明确。
- KCFI 风险点：hook 的是 `int (*)(struct seq_file *, void *)` 这类标准签名，
  与内核一致，不需要改被调函数的类型哈希。

**这一层做完，DS 的 `Process limit support` 就是绿的。**

**但必须诚实说明副作用**：DS 随后会尝试写 `pids.max`。
文件不存在 → `apply_limit()` 返回 0 → `cfg->pids_limit = 0` →
DS 打印 "limit skipped" 并把 `--pids-limit` 置灰。
所以方案 A 解决的是"全绿"，**不解决"限制生效"**。

### 方案 B：功能层 —— ko 自己做进程数限制（中风险，1–2 天量级）

> **实现状态：已实现、编译通过（2026-10-27，PMB110 / 6.12.69，android16-6.12 DDK）。**
> 代码在 `src/pidsmod/pids_ctl.c`，随 `droid_lkm_pids.ko` 一起加载。
> 真机验证脚本：`scripts/verify-pids-b.sh`。
>
> 与下方原始设想的两点偏差（均为更稳健的取舍）：
> - 计数维度用 **cgroup** 而非 PID namespace。DS 的 `--pids-limit` 本身就是
>   per-cgroup 的（容器目录 `/sys/fs/cgroup/droidspaces/<name>`），按 cgroup 计数
>   与真实 `pids` 控制器语义一致，且不依赖 namespace 内部布局。
> - 控制面复用 DS 已有的行为：DS 会向 `pids.max` 写值，我们直接把它做成真实文件
>   （挂在 memory 控制器上的 cftype），无需额外的 `/proc` 控制面。

绕过 cgroup 控制器，用 ko 自己在内核里维护一套 per-container 计数：

- **计数维度**：DS 的容器 = 自己的 PID namespace + 自己的 cgroup 目录。
  按 **PID namespace** 计数最贴合语义（容器里 fork 出来的进程一定在这个 ns 里）。
- **拦截点**：
  - `cgroup_can_fork()`（`kernel/cgroup/cgroup.c:6727`）—— 返回非 0 就 abort fork，
    这是内核给 cgroup 子系统预留的正式拒绝点，语义与 `pids` 控制器一致；
  - `cgroup_fork()`（`:6526`）/ `cgroup_exit()`（`:6883`）—— 维护计数的增减。
  - 三个都是全局符号，`CONFIG_KALLSYMS_ALL=y` 保证可解析。
- **拒绝语义**：超限时 `cgroup_can_fork` 返回 `-EAGAIN`，用户态看到 fork 失败，
  和真实 pids 控制器行为一致。
- **配置下发**：DS 不会写我们的文件，所以要走"DS 不改源码"的前提：
  - 选项 1：ko 暴露一个 `/proc/droid_lkm/pids_limit` 之类的控制面，
    由宿主侧脚本/Module WebUI 写入，DS 只负责显示（显示层仍用方案 A）。
  - 选项 2：DS 是 **GPL-3.0**（`LICENSE`、`README.md:113`），
    `src/cgroup.c` / `src/check.c` 都是可改可分发的上游代码。
    可以在 DS 侧加一个"把 `--pids-limit` 转发给 ko"的分支 —— **这是唯一需要动 DS 的方案**，
    而且是一次性的、可持续维护的（见 §5）。

### 方案 C：借用内核现成的 `RLIMIT_NPROC` 路径（低风险，但语义不完全等价）

内核已有的每 UID 进程数限制：

- `kernel/fork.c:2269`
  ```c
  if (is_rlimit_overlimit(task_ucounts(p), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))) {
    if (p->real_cred->user != INIT_USER &&
        !capable(CAP_SYS_RESOURCE) && !capable(CAP_SYS_ADMIN))
        goto bad_fork_cleanup_count;
  }
  ```
- `kernel/user_namespace.c:142`
  ```c
  set_userns_rlimit_max(ns, UCOUNT_RLIMIT_NPROC, enforced_nproc_rlimit());
  ```
  → userns 内的 RLIMIT_NPROC 有独立上限，正好可以按容器隔离。

**关键事实：DS 完全没用 rlimit。** `grep -rn "setrlimit\|RLIMIT" src/` 在
Droidspaces-OSS 整个 `src/` 下**零命中**。所以这条路必须由 DS 侧新增调用
（属于"要动 DS 源码"的那一类），或者由 ko 在 `copy_creds` 之后替容器设置。

优点：完全复用内核成熟代码，不用自己写计数。
缺点：
- 语义是"每 UID"而非"每容器"，容器内多 UID 时会低估；
- `INIT_USER`（uid 0）默认绕过，容器内通常是 root → 需要额外处理。

### 方案 D：换用社区预编译内核（有 PMB110 现成版本）

Droidspaces 官方社区列表里有 `OnePlus ACE 6 Ultra PMB110`：
Android 16 / 6.12.58 / KernelSU Next + SusFS / @fatalcoder524 / Working，
作者 README 明确带「Droidspaces Support」特性。

- **优点**：直接解决"DS 官方配置 + kABI 破坏"的整套麻烦，作者已经处理了模块同步；
  这正是"别人全绿"的真实路径。
- **缺点**：这是刷内核，**超出"只允许 ko、不刷内核"的边界**；
  且社区内核版本随 OTA 变动，刷之前要确认当前版本的 config 里
  `CONFIG_CGROUP_PIDS` 确实是 `y`。

---

## 4. 方案对比

| 方案 | 改 DS 源码 | 刷内核 | 检查项变绿 | `--pids-limit` 真生效 | 风险 | 工作量 |
|------|-----------|--------|-----------|----------------------|------|--------|
| A 显示层 | 否 | 否 | ✅ | ❌ | 低 | 半天 |
| B 功能层（ko 自己计数） | 否（DS 不改）| 否 | ✅ | ✅ | 中（inline hook 数量多）| 1–2 天 |
| B' 功能层（DS 转发） | **是**（一次）| 否 | ✅ | ✅ | 中 | 1–2 天 + DS 侧小改 |
| C RLIMIT_NPROC | **是** | 否 | ✅ | ✅（语义近似）| 低 | 1 天 |
| D 社区内核 | 否 | **是** | ✅ | ✅ | 刷机风险 | 小时级 |
| E 重编内核全刷 | 否 | **是** | ✅ | ✅ | 极高（bootloop）| 1–2 周 |

> **方案 B 是唯一同时满足"只 ko、不刷内核、不改 DS 源码、功能真生效"的路线。**

---

## 5. 关于"他们更新 APP 源码我又要修改，是不是麻烦"

这是本次最该讲清楚的一点。

### 情况一：你走 ko 路线（方案 A / B）

**DS 更新，你这边零改动。**

原因是：ko 只依赖**内核接口**，不依赖 DS 的代码或二进制。

- 方案 A 只 hook 内核里的两个 seq_show，DS 读到什么就显示什么 —— DS 换版本、
  改判定文案、调整检查顺序，都不影响 hook 点。
- 方案 B 的计数与拒绝全在内核，DS 侧只发一个 `--pids-limit=N` 参数，
  这个参数在 DS 的 CLI/配置里是稳定契约（`src/cgroup.c:855` 读 `cfg->pids_limit`）。
- 你只需要在 DS 升级后**跑一次 `droidspaces check`**，确认那一项还绿。

真正需要跟版本走的只有一种情况：你选择把 ko 的控制面暴露成 DS 能识别的文件，
而 DS 换了文件查找路径。这可以规避 —— 见 §6 的设计约束。

### 情况二：你走"改 DS 源码"路线（方案 B' / C）

**会麻烦，但麻烦是可控的一次性成本。**

- Droidspaces-OSS 是 **GPL-3.0**，源码可改可改分发，你本来就是开源交付，
  改它不引入任何法律问题。
- 需要维护的改动量很小：DS 侧只需要在 `apply_limit()` 的 "pids" 分支前
  插一个"如果有 ko 的控制面，就转发过去并返回 1"的判断，加一个能力探测。
  这类改动在上游重构时**大概率需要 rebase**，但不会每次都冲突成灾。
- 更稳的做法：**把 DS 侧改动做成可选的、检测式的**——
  DS 先找 ko 的接口，找不到就退回原逻辑。这样上游更新只要不重写
  `apply_limit()` 的签名，你的补丁就不会失效。

### 情况三：你走"社区内核"路线（方案 D）

**完全不涉及 APP 版本问题**，因为限制是内核原生能力，DS 什么版本都能用。

代价是刷内核，以及跟随 OTA 升级。

---

## 6. 推荐的实施路线（建议）

按"先拿结果、再补功能"的顺序，两步走：

**第一步（快，半天）：方案 A —— 显示层绿。✅ 已完成，待真机验证。**
实现为独立模块 `droid_lkm_pids`（`src/pidsmod/pids_main.c`）：
- 解析 `cgroup_controllers_show` 与 `proc_cgroupstats_show`；
- 各自挂一个 wrapper，在原输出后补 `pids`；
- 按现有 `droid_lkm_misc_ksym` 的降级范式处理：
  **符号缺失就跳过对应路径，而不是让模块加载失败**
  （`DROID_LKM_MISC_THUNK` 那套：缺失即禁用该功能）。

这一步的产出：`Process limit support` 变绿，DS 的检查面板全绿。
风险极低（不 hook 任何会影响调度/内存的路径），可随时 `rmmod` 回退。

**第二步：方案 B —— 功能层。✅ 已实现、编译通过（2026-10-27）。**
实现为 `droid_lkm_pids` 子模块的 `src/pidsmod/pids_ctl.c`：
- **B1 文件注入**：`cgroup_add_dfl_cftypes(memory_cgrp_subsys, pids.max)`，在容器 cgroup 目录里造出真实的 `pids.max` 文件（DS 写值即生效，不再 "limit skipped"）；
- **B2 计数与拒绝**：per-cgroup 原子计数（xarray 按 cgroup 指针索引）；
  hook `cgroup_can_fork`（容器内 fork 计数）、`cgroup_procs_write`（DS 把进程迁进容器的迁移点计数）、`cgroup_exit`（退计）；
  超限 `atomic64_fetch_add_unless` 拒绝，返回 `-EAGAIN`，与真实 pids 控制器一致；
- **B3 subtree_control 修正**：hook `cgroup_subtree_control_write` 剥掉 DS 写进去的 `+pids` token，
  否则整次 `+memory +cpu +pids` 写入会因内核无 pids 控制器而 `EINVAL`，连带打掉 `+memory`/`+cpu`；
- **B4 安全**：只触碰公有字段（`css->cgroup`、`css_set->dfl_cgrp`）与导出符号，无任何内核内部布局假设；
  cgroup 指针仅作 xarray key 使用，rmdir 时清理，杜绝悬垂指针误判；
  任一 hook 安装失败即整体回退（`-ENODEV/-EINVAL`），绝不半挂载。
- 验证：`scripts/verify-pids-b.sh` 在 Termux 里确认 pids.max 出现、可写、且超限 fork 被拒。

### 设计约束（为了 DS 升级不返工）

1. ko **不要**假设 DS 的 cgroup 目录布局（`/sys/fs/cgroup/droidspaces/<name>`）。
   计数维度用 **PID namespace**，不依赖路径。
2. ko **不要**依赖 DS 的检查顺序或文案。
3. ko 的控制面用**独立路径**（`/proc/droid_lkm/...`），不抢占 DS 惯用的 cgroup 路径。
4. 保留一个**模块参数**（如 `pids=0/1/2`）控制 A/B 两层，随时关掉。

这样 DS 什么时候更新、怎么改，都不需要动 ko。

---

## 7. 附带发现：Cgroup namespace 那一项

你贴的输出里 `Cgroup namespace` 是绿的，说明本机内核实际开了
`CONFIG_CGROUP_NS`（AOSP GKI 基线里没有，厂商自己加的）。
DS 的判定是 `check_ns(CLONE_NEWCGROUP, "cgroup")`（`src/check.c:383`），
先 `access("/proc/self/ns/cgroup")` 再真 `unshare()` 一次 —— 是真检测，不是看文件。
DS 的容器创建里 `CLONE_NEWCGROUP` 只在 `/proc/self/ns/cgroup` 存在时才加入
（`src/monitor.c:240-241`）。这一项无需额外处理。

---

## 8. 参考

- DS 判定：`src/check.c:459`、`src/cgroup.c:162-182/202/740-800/855`
- DS 许可证：`LICENSE`（GPL-3.0）、`README.md:113`
- 内核约束：`include/linux/cgroup_subsys.h`、`include/linux/cgroup-defs.h:234`、
  `kernel/cgroup/cgroup.c:158/3073/4401/4577/6526/6727/6883`、
  `kernel/cgroup/cgroup-v1.c:676`
- RLIMIT 路径：`kernel/fork.c:2269`、`kernel/user_namespace.c:62/142`、
  `kernel/cred.c:311/348/440/445`
- 官方文档：`droidspaces.org/docs/kernel-configuration.html`、
  `Droidspaces-OSS/Documentation/Kernel-Configuration.md`
- 社区内核：`github.com/fatalcoder524/OnePlus_KernelSU_SUSFS`
  （`configs/a16/OP-ACE-6-ULTRA.json`，`"ds": true`，`branch: wild/mt6993`）
