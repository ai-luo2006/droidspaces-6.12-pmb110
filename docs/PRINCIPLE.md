# 实现原理（开源说明）

本文说明每个模块**到底做了什么**，方便审阅者判断风险与可移植性。

---

## 0. 目标内核缺什么

一加 Ace 6 至尊版用的 GKI 6.12 内核对 Android 做了裁剪：

| 配置 | 状态 | 影响 |
| --- | --- | --- |
| `CONFIG_PID_NS` | n | 容器看不到独立 PID 空间 |
| `CONFIG_IPC_NS` | n | System V IPC / POSIX mqueue 无法隔离 |
| `CONFIG_SYSVIPC` | n | `ipcs` 不可用 |
| `CONFIG_POSIX_MQUEUE` | n | mqueue 不可用 |
| `CONFIG_USER_NS` | n | 容器内无法映射 uid/gid。LKM 只能在 syscall 边界做 id 翻译，**不构成隔离**，容器 root 仍是宿主 root（见 1.3 与 `CHANGELOG.md`） |
| `CONFIG_DEVTMPFS` | n | 容器内 `/dev` 拿不到真实设备节点，"硬件访问"开不了 |
| `CONFIG_MACVLAN` | n | 容器拿不到独立局域网 IP |
| `CONFIG_IP_NF_NAT` / ip6 nat | 部分缺失 | `--net=nat` 的 IPv6 不通 |
| `CONFIG_CGROUP_PIDS` | n | `--pids-limit` 原生不可用；本仓库用 LKM 模拟出来了（见 `PIDS-ENFORCEMENT.md`） |

不刷内核的前提下，只能用 LKM 在运行期补齐。

---

## 1. 三个主模块

### 1.1 `droid_lkm.ko` —— namespace / IPC

核心手法：

1. **Type-info BTF**：内核带 BTF 时，可用 BTF 描述在运行期重建结构体布局，
   无需硬编码偏移量（这是跨内核版本可移植的关键）。
   源码：`droid_lkm/src/main/core/ds_ti.c`。
2. **符号解析**：`droid_lkm/src/main/core/ds_ksym.c` 用 KallRecon 从内核
   镜像恢复 kallsyms 表，按名字取地址，绕开 `CONFIG_TRIM_UNUSED_KSYMS=y`
   和 `kptr_restrict=2`。
3. **系统调用改写**：KernCall 改写 syscall 表，把 `unshare`、`clone`、
   `fork` 等入口换成自己的实现，在其中建立/切换 namespace。
   源码：`droid_lkm/src/main/ipcns/`。
4. **IPC 虚拟化**：为每个 namespace 维护独立的 SysV IPC 与 mqueue 视图
   （`droid_lkm/src/main/ipcns/sysv/`、`mqueue/`）。

模块参数（实际使用的）：

```
gate=0            主功能开关
mqueue=1          启用 POSIX mqueue
nspid=1           启用 PID namespace
nspid_append=1    pid 追加模式
skip_sysvipc=0    启用 System V IPC
no_fake_ns=0      不使用伪 namespace
skip_do_exit=0    启用 do_exit 路径处理
```

### 1.2 `droid_lkm_vendor.ko` —— 厂商兼容

OPPO/一加内核的厂商改动会让标准 hook 失效，这个模块做两件事：

- **ghost task**：`find_task_by_vpid` 在厂商内核上找不到某些 pid，
  用一张自建表兜底（`droid_lkm/src/compat/vendor/ghost.c`）。
- **ddl 历史表**：厂商的 ddl 记录结构随版本变动，用运行期探测选择布局
  （`droid_lkm/src/compat/vendor/ddl_hit.c`）。

参数：

```
ghost=1             启用 ghost task 兜底
ghost_match=oplus_  pid 前缀匹配规则（只对 oplus 前缀的 pid 建表）
pid_max_cap=0       不限制 pid 上限
```

### 1.3 `droid_lkm_misc.ko` —— userns 视图（本机不生效）+ devtmpfs

**user namespace**（`droid_lkm/src/misc/userns/`）：

> **结论先行**：本机内核未编译 `CONFIG_USER_NS`，这一层代码保留但**不产生任何隔离**。
> 证据链见 `CHANGELOG.md` 末尾。下面记录它曾经做什么、以及为什么走不通。

`CONFIG_USER_NS=n` 的实际后果：

- `kernel/user_namespace.c` 整个文件不编译，`create_user_ns()` 在内核里不存在
- `get_user_ns` / `put_user_ns` / `make_kuid` / `from_kuid` 全部是 `static inline`
  恒等实现，编译进调用点，没有函数地址
- `ksys_unshare()` 里 `CLONE_NEWUSER` 直接 `return -EINVAL`
- `/proc/<pid>/ns/user` 不存在，`/proc/<pid>/uid_map` 恒为 `0 0 4294967295`

所以模块只能做两件事，**都不构成隔离**：

1. **unshare 假成功** —— 拦到 `CLONE_NEWUSER` 时剥掉该 flag，其余命名空间位交给内核。
   调用方（Droidspaces App）据此认为"沙箱可用"，容器本身仍在宿主 init user ns 里跑。
2. **id 翻译视图** —— `getuid/geteuid/getgid/getegid` 出口改写返回值；
   `setuid/setresuid/setreuid/setfsuid/chown/setgroups` 入口把容器 id 换成宿主 id；
   `vfs_getattr` 改写 `struct kstat` 的 uid/gid。
   这些钩子在 `real_userns=1` 时武装，但没有任何进程真的在 userns 里，
   `droid_lkm_userns_of()` 找不到对象，全部退化成直通。

> 为什么不去改 `from_kuid()`：`CONFIG_USER_NS=n` 时它是 `static inline` 恒等函数，
> 内联进上千个调用点，LKM 拦不到——这正是上面只能做出口/入口翻译的原因，
> 也是"翻译"永远不等于"隔离"的原因。

**已废弃、不要再碰的路径**（v1.0.19 起删除，`CHANGELOG.md` 有记录）：

- 伪造 `struct user_namespace`（自己 `kmalloc`）并挂到 cred 上
- 在 `clone` / `fork` 时替换 cred 的 `user_ns`，用 `kernel_clone`、`do_execve` 抢命名空间
- `userns_adopt=1` 参数与 `$MODDIR/userns_adopt` 文件

`cap_capable()` 中遍历 `cred->user_ns->parent` 的代码没有 `#ifdef CONFIG_USER_NS`，
一直参与编译；cred 指向伪造结构时，会读 `->level`、`->parent` 的野指针 →
内核 panic / 整机重启。v1.0.18 的那次重启就是这个原因。

**历史遗留代价**：如果曾经 relabel 过，rootfs 属主会停在 `base`（默认 200000），
必须 `sh userns.sh restore all` 改回 0，否则容器读不动自己的文件
（表现为 `su: cannot set groups`）。现在 `relabel` 已被护栏拦住，除非显式 `--force`。

**devtmpfs**（`droid_lkm/src/misc/devtmpfs/devtmpfs.c`）：

内核 `CONFIG_DEVTMPFS=n`，但 `/proc/filesystems` 里要出现 `devtmpfs`，
Droidspaces 才会认为支持。

做法：用 `shmem` 自建一个名为 `"devtmpfs"` 的文件系统并注册进内核
（`register_filesystem`）。**只注册**——不起后台线程、不 hook 任何路径、
不碰 dentry/inode。挂载与节点填充全部由 Droidspaces 自己完成。

> 早期版本在注册后 `dget(mnt_root)` 拿 superblock 的 root dentry，
> 在这个时序下可能为空指针 → panic。现在完全不碰。

---

## 2. `macvlan.ko`

### 2.1 为什么不能直接编内核的 macvlan

目标内核 `CONFIG_MACVLAN=n`，但源码树里有 `if_macvlan.h`。
把内核 `drivers/net/macvlan.c` 拿来当 out-of-tree 模块编，会遇到三件事：

1. **未导出符号**：以下 7 个函数在内核里存在但没有导出条目
   （`CONFIG_TRIM_UNUSED_KSYMS=y`），链接直接报 `Unknown symbol`：

   ```
   dev_forward_skb        __dev_forward_skb     dev_mc_add_excl
   ip_check_defrag        ndo_dflt_fdb_dump     eth_header_cache
   eth_header_cache_update
   ```

2. **`if_macvlan.h` 的 BUG() 存根**：当 `CONFIG_MACVLAN` 未定义时，
   `linux/if_macvlan.h:82` 里的静态 inline 函数会 `BUG()`。
   → 必须加 `-DCONFIG_MACVLAN=1`。

3. **KCFI**：签名必须与内核编译时一致，用错 clang 直接关机。

### 2.2 shim 方案

`net_macvlan/macvlan_shim.h` 用宏把上面 7 个符号改名：

```c
#define dev_forward_skb         droid_mv_dev_forward_skb
#define __dev_forward_skb       droid_mv___dev_forward_skb
#define dev_mc_add_excl         droid_mv_dev_mc_add_excl
#define ip_check_defrag         droid_mv_ip_check_defrag
#define ndo_dflt_fdb_dump       droid_mv_ndo_dflt_fdb_dump
#define eth_header_cache        droid_mv_eth_header_cache
#define eth_header_cache_update droid_mv_eth_header_cache_update
```

因为用 `-include` 强制插在 `macvlan_core.c` 所有 include 之前，
**内核头文件里的 extern 声明也会一并改名**，原型由编译器校验，签名不会错。

`macvlan_shim.c` 里：

- 每个 `droid_mv_*` 是一个**地址槽**（`void *`）。
- 模块 init 时用 **KallRecon** 从内核 kallsyms 表按名字查真实地址，填进槽。
- 真实实现体（`macvlan_core.c` 里被改名的原函数）通过槽调用真正的内核函数。

这样就**不依赖导出表、不依赖 `kptr_restrict`、不受 KASLR 影响**
（KASLR 地址每次开机都变，所以必须运行期解析，不能编译期写死）。

### 2.3 数据面限制

本机 WiFi 驱动只接受一个 MAC，macvlan 子接口发出的帧驱动直接丢弃：
实测 `TX` 计数上涨、`RX` 恒为 0、ARP 永远 `FAILED`。
这是**驱动限制，不是模块 bug**。USB 有线网卡下可用。
host 网络模式不需要 macvlan。

---

## 3. `ip6table_nat.ko`

同构方案，只是符号更多（9 个）：

```
ip6t_alloc_initial_table   ip6t_register_table        ip6t_unregister_table_exit
ip6t_do_table              xt_find_table              xt_register_template
xt_unregister_template      nf_nat_ipv6_register_fn    nf_nat_ipv6_unregister_fn
```

shim 在 `net_ip6nat/ip6t_nat_shim.{c,h}`。

> 坑：`ip6t_register_table()` 第 4 个参数在 6.12 上必须是
> `const struct nf_hook_ops *`，类型写错能编过但行为不对。

实现效果：`ip6tables -t nat -L` 可列出，`MASQUERADE` 规则可增可删。

---

## 4. 加载与守卫（`dsm/post-fs-data.sh`）

加载顺序很重要：

```
droid_lkm  →  droid_lkm_vendor  →  droid_lkm_misc  →  macvlan  →  ip6table_nat
```

守卫链：

1. **首次安装自动关闭自启**（写 `no_boot`），保证"装了不会变砖"。
2. **bootloop 守卫**：insmod 前写 `insmod.inflight`，成功后删。
   下次开机若发现该文件还在 → 说明崩在 insmod → 写 `blocked` 并跳过。
3. **存活守卫**：insmod 成功后写 `boot_pending`；`service.sh` 等 120 秒，
   确认系统还活着才写 `.boot_ok`。下次开机若 `boot_pending` 在而
   `.boot_ok` 不在 → 判定"加载后崩溃" → 写 `blocked`。
4. **机型 + 内核双锁**：内核必须 `6.12.69-*`，机型必须含 `PMB110`。

网络模块的加载有**两级回退**：

- 首选：模块自解析地址（KallRecon），`insmod macvlan.ko` 即可。
- 回退：自解析失败时，脚本临时把 `kptr_restrict` 置 0，从
  `/proc/kallsyms` 读出 7/9 个符号地址，以 `addr_<sym>=0x...` 形式传入。

---

## 5. WebUI（`dsm/webroot/index.html`）

纯前端单文件，通过 KernelSU 提供的接口执行命令：

```js
window.ksu.exec(cmd, "{}", callbackName)
```

三页：状态 / 日志 / 诊断。状态页对每个模块显示加载状态与独立开关，
支持加载 / 卸载 / 开机自启 / 延迟加载 / 存活窗口 / 解阻断。

---

## 6. 风险声明

- 这些模块修改了内核的 namespace 与 IPC 视图，**属于非常规操作**。
  内核崩溃会导致手机重启或数据丢失。
- 本项目**仅在 PMB110 / 6.12.69 一套组合上验证过**。
- 所有模块都有守卫，**默认不自动加载**是刻意的设计，请先手动验证。
- 不要在主力机上开启开机自启，除非你真的需要容器功能并且能接受风险。
