# pids 控制器功能层（Plan B）

> 目标：让 Droidspaces 的 `--pids-limit` **真的限制住容器**，而不是只在检查项里打勾。
>
> 现状：目标内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` 未编译
> `CONFIG_CGROUP_PIDS`，内核里没有 pids 子系统。LKM 不能凭空加一个 cgroup
> 子系统，但可以把它**模拟**出来：一个能读写的 `pids.max` 文件 + 一条会拒绝
> `fork()` 的路径。

代码位置：`droid_lkm/src/pidsmod/`

| 文件 | 职责 |
| --- | --- |
| `pids_main.c` | 显示层：让内核把 `pids` 这个名字打印出来 |
| `pids_ctl.c` | 功能层：`pids.max` 文件 + fork 拦截 + `subtree_control` 翻译 |

---

## 1. 两层，各解决一半问题

Droidspaces 侧的失败分两步：先是**找不到** pids 控制器（打印
`pids.max is not available`，检查项打 ✗），然后才是**限制不生效**。

### 显示层（A）

Hook 两个只负责"打印"的函数：

- `cgroup_controllers_show` → `cgroup.controllers` 里出现 `pids`
- `proc_cgroupstats_show` → `/proc/cgroups` 里出现 `pids 0 1 1`

Droidspaces 只核对控制器**名字**，所以这一步就能让 `Process limit support`
检查项变绿。它不改动任何行为，是纯显示。

### 功能层（B）

| 编号 | 问题 | 解决 |
| --- | --- | --- |
| B1 | 没有 `pids.max` 可写 | 挂到 memory 控制器上造一个 |
| B2 | 写了也不限制 | kprobe 拦截 fork 并返回 `-EAGAIN` |
| B3 | DS 的 `+pids` 使整个 `subtree_control` 写失败 | 把 `+pids` 翻译成 `+memory` |

---

## 2. B1：`pids.max` 是怎么造出来的

内核没有 pids 子系统，但 `cgroup_add_dfl_cftypes()` 允许给**已存在的**子系统
追加控制文件。memory 是 Droidspaces 一定会启用的控制器，于是：

```c
ret = cgroup_add_dfl_cftypes(memory_ss, pids_max_files);
```

`pids_max_files` 里三个文件都带 `CFTYPE_NO_PREFIX | CFTYPE_NOT_ON_ROOT`，
所以名字是字面量，不会变成 `memory.pids.max`：

| 文件 | 权限 | 作用 |
| --- | --- | --- |
| `pids.max` | rw | 接收 Droidspaces 写入的 limit，存进 per-cgroup 的 `limit` |
| `pids.current` | ro | 层级累加的当前计数（含后代 cgroup） |
| `pids.stats` | ro | 探针自检计数器，见 §6 |

per-cgroup 状态放在一把 xarray 里，key 是 `struct cgroup *`：

```c
struct pids_priv {
	atomic64_t counter;	/* 本 cgroup + 所有后代 */
	atomic64_t limit;	/* 0 = 未启用；S64_MAX = "max" */
};
```

memory 控制器被禁用时 `cgroup_add_dfl_cftypes()` 返回 0 且什么都不加，属于
优雅降级。

---

## 3. B2：fork 拦截

### 3.1 记账点

| kprobe | 动作 |
| --- | --- |
| `cgroup_can_fork` | 超限 → 写 `-EAGAIN` 进 x0 并直接返回，函数体不执行 |
| `cgroup_post_fork` | 记账 `+1` |
| `cgroup_exit` | 记账 `-1` |
| `cgroup_rmdir` | 清掉该 cgroup 的状态，避免指针复用后沿用旧 limit |

`pids_charge()` / `pids_check()` 都沿 `cgroup->self.parent` 往根走，所以
容器目录上设的 limit 约束整棵子树——真实 pids 控制器就是这个语义（容器的进程
实际活在 `ds-enter` / `init.scope` / `system.slice` 等子 cgroup 里）。

### 3.2 拒绝是怎么送达的

kprobe 是"观察"而不是"替换"，所以不能改函数体。做法是把返回值写进 `x0`
再走链接寄存器返回，让被调用函数在本次调用里根本不执行：

```c
atomic_inc(&ctl_n_refused);
regs->regs[0] = (unsigned long)(-EAGAIN);
hk_regs_return(regs);
return 1;
```

`copy_process()` 收到 `-EAGAIN` 后按正常失败路径展开，`fork()` 系统调用向
用户态返回 `EAGAIN`，和真实 pids 控制器超限时的表现一致。

### 3.3 记账点踩过的三个坑

这一段是本项目最花时间的地方，写下来免得重复踩：

1. **在 `cgroup_can_fork` 里记账 → 计数只涨不落。**
   失败的 fork 走 `cgroup_cancel_fork()`，而它遍历的是内核自己的子系统链表，
   永远到不了模块。所以 charge 必须挪到"确实创建成功"的 `cgroup_post_fork`。

2. **在 `cgroup_post_fork` 里读 `child->cgroups` → 恒为 0。**
   pre-handler 在函数入口触发，而 `rcu_assign_pointer(child->cgroups, cset)`
   在函数体里、几行之后才执行，此时该字段还是子进程出生时的旧值。
   最终改读 `kargs->cset`——它在更早的 `cgroup_can_fork()` →
   `cgroup_css_set_fork()` 里已经填好。

3. **计数只算本层 → 容器显示 `current=1`。**
   改成沿祖先链累加后才是真实 pids 语义。

### 3.4 为什么是 kprobe，不是 inline hook

- `cgroup_procs_write` 的 inline hook trampoline 里含 `BRK #4`
  （`d4200080`）。ksud 一写 `cgroup.procs` 就踩上去，**加载后 22ms 整机断电**。
  该 hook 已彻底删除，并加了加载期 trampoline 陷阱自检。
- 本文件里所有"被直接调用"的目标，inline hook 装得上但**一次都不触发**
  （`cgroup.subtree_control` 照旧拒绝 `+pids`，`pids.current` 数百次 fork 后仍是 0）。
- `register_kprobe()` 在 `text_mutex` + `cpus_read_lock` 下打断点，不依赖
  重定位目标函数入口，是内核自己维护的机制。

### 3.5 结构体布局自校验

`struct css_set` 以 `subsys[CGROUP_SUBSYS_COUNT]` 开头，所以
`dfl_cgrp` 的偏移取决于运行内核编了几个控制器。模块在运行时从
`cgroup_subsys[]` 数出实际个数并换算偏移，然后**双重校验**：

```
init_css_set.dfl_cgrp == &cgrp_dfl_root.cgrp
init_task.cgroups     == &init_css_set
```

任一不符 → `ctl_layout_ok = false` → **fork / exit / rmdir 探针一个都不装**，
只保留显示层。偏移错了的代价是功能缺失，不会去读随机内存。

---

## 4. B3：`subtree_control` 翻译

Droidspaces 为了拿 `pids.max`，会往容器 cgroup 的 `cgroup.subtree_control`
写 `+memory +cpu +pids`（或只写 `+pids`）。内核不认识 pids，整条写失败并返回
`-EINVAL`，**把 memory 一起拖下水**——这正是容器目录里一直没有 memory、
因而也没有 `pids.max` 的原因。

`cgroup_subtree_control_write` 的 kprobe 在内核解析 buffer 之前动手：

1. 把 `pids`、`cpu` 这两个内核没有的 token 抹成空格（保留 `+`/`-` 前缀一起抹，
   且要求 token 边界，避免把 `cpuset` 削成 `set`）；
2. 抹完若还有内容（如 `+memory`）→ 交给内核照常解析；
3. 抹完若空了、且原始请求里有 `+pids` → 把 buffer 换成 per-CPU 的 `"+memory"`，
   并改写 `regs[1]` / `regs[2]`。因为 kernfs 传进来的 buffer 是按用户写入长度
   分配的，`"+pids"` 只有 6 字节，写不下 `"+memory"`。

**每个 cgroup 只替换一次**：memory 一旦 enable 就是粘性的，重复 enable 会让
Droidspaces 收到 `Bad address`（EFAULT）。

---

## 5. 完整链路

```
DS 创建容器
  └─ 写 cgroup.subtree_control "+memory +cpu +pids"
       └─ [kprobe] 翻译成 "+memory"          ← B3
            └─ 容器 cgroup 拿到 memory css
                 └─ pids.max 文件出现          ← B1

DS 读 cgroup.controllers / /proc/cgroups → 看到 "pids"   ← 显示层

DS 写 pids.max = 256                                     ← B1
  └─ 存进 xarray

容器 fork
  └─ cgroup_can_fork → [kprobe] 超限？ → -EAGAIN         ← B2
  └─ cgroup_post_fork → [kprobe] counter +1
  └─ cgroup_exit      → [kprobe] counter -1
```

---

## 6. `pids.stats`：自检计数器

```
subtree=3644 strip=48 rewrite=36 fork=10530 charged=10530 refused=629 rmdir=378 layout=1
```

| 字段 | 含义 | 判据 |
| --- | --- | --- |
| `subtree` | 到达探针的 `subtree_control` 写次数 | > 0 说明探针在跑 |
| `strip` | 抹掉的非法 token 数 | — |
| `rewrite` | 整条请求被换成 `+memory` 的次数 | — |
| `fork` | `cgroup_can_fork` 触发次数（**全系统**，不只是容器） | 持续增长 = 探针活着 |
| `charged` | 其中目标 cgroup 解析成功的次数 | 与 fork 接近 |
| `refused` | **被拦下并返回 `-EAGAIN` 的 fork 次数** | **> 0 即功能层生效** |
| `rmdir` | cgroup 目录回收次数 | — |
| `layout` | 布局自校验结果 | 必须为 1 |

这个文件的存在意义是把两种失败区分开：**探针没装** vs **探针装了但全放行**。
`fork` 不动说明探针打在了内核永远走不到的地址上（日志里看起来完全正常）。

---

## 7. 真机验证记录

设备：一加 Ace 6 至尊版 `PMB110`，`17.0.0.101(CNO1C10P02)`，
内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`，2026-10-28。

### 7.1 Droidspaces 侧

- 不再打印 `pids.max is not available`
- 容器目录里出现 `pids.max` / `pids.current` / `pids.stats`
- `Process limit support` 检查项通过

### 7.2 手工 cgroup 对照实验

```sh
su -c 'CG=/sys/fs/cgroup/t2; rmdir $CG 2>/dev/null; mkdir -p $CG; echo 5 > $CG/pids.max; echo $$ > $CG/cgroup.procs; read S < $CG/pids.stats; echo "start   $S"; i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); read C < $CG/pids.current; read S < $CG/pids.stats; echo "第${i}次 current=$C  $S"; done; rmdir $CG 2>/dev/null'
```

实测输出：

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

读法：

- `current` 涨到 4 就上不去了，`refused` 在第 4→5 次之间 **+5**。
- 第 4 次到第 5 次之间有约 6 秒停顿，期间 `fork` 增加 58（全系统）。
  这不是 hang：`/system/bin/sh` 在 `fork()` 返回 `EAGAIN` 时会**等待后重试**，
  直到第一批 `sleep 6` 到期、配额释放才成功——于是 `current` 掉回 1 重新涨。
- `refused` 基线 624 来自**正在运行的真实容器**（limit=80），说明功能层对
  DS 容器同样在持续拦截。

### 7.3 用户侧对照实验

| `--pids-limit` | 现象 |
| --- | --- |
| 32 | 容器 stop 超时，最终被 SIGKILL（配额太小，容器自身起不来） |
| 80 | `docker stop` 干净退出 |

同一个容器只改 limit，一次停不掉一次正常停——说明拦截确实落在 fork 路径上。

### 7.4 稳定性

`dmesg` 无 `Unable to handle kernel paging request` / `panic` / `BUG:` / `CFI failure`。
五个探针全部安装成功：

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

## 8. 已知偏差与限制

1. **迁移进来的进程不立即计入。**
   计数只统计"在 cgroup 内 fork 出来的"进程。通过写 `cgroup.procs` 迁移进去的
   那一个不计入（真实控制器会立即计入）。所以 `pids.max=5` 实际能容纳 5 个进程，
   而 `pids.current` 只到 4。真实场景 `--pids-limit=256` 时这 1 个的差异无影响。

2. **`--pids-limit` 建议 ≥ 256。**
   空跑的容器自身约需 68 个进程。设 32 会把容器饿死（表现为 stop 超时后被 SIGKILL）。

3. **重复 `enable` 会 EFAULT。** B3 已按 cgroup 去重（`CTL_REWRITTEN_MAX = 8`
   个槽位）。槽位用满后不再替换，此时若 DS 重发 `+pids` 会拿到 `Bad address`，
   属预期降级。

4. **`pids.current` 是层级累加值**（本 cgroup + 所有后代），与真实语义一致，
   不是"该目录直属进程数"。

5. **只在本机型/内核验证。** 换内核需重编，且布局自校验失败时功能层会
   自动退化成纯显示层（不会崩，只是不限制）。

6. **每个探针都是 best effort**：装不上就跳过，内核保持原样，功能缺失但绝不崩。
