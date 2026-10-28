# 变更记录

> 版本状态标记：🟢 稳定可用 · 🟡 可用但有已知问题 · 🟠 半成品 · ⚪ 无效/过时 · 🔴 会导致整机重启

## 版本总览

| 版本 | 状态 | ko md5 前缀 | 一句话 |
|---|---|---|---|
| v1.0.20 | 🟢 | `15fc08cb` | WebUI 修正，禁用危险按钮（**当前版本**） |
| v1.0.19 | 🟢 | `15fc08cb` | 删掉换凭据路径，止住重启（**稳定基线**） |
| v1.0.18 | 🔴 | `25583035` | `kernel_clone` 钩子，两次整机重启 |
| v1.0.17 | 🟠 | — | 建成 mnt ns，但子进程凭据未切换 |
| v1.0.16 | ⚪ | — | 挂错钩子（fork），从未触发 |
| v1.0.15 | 🟡 | — | 卸载引用检查，防 UAF；加 `fake_config.sh` |
| v1.0.14 | 🟡 | — | 非 root `unshare` 假成功，App 探测过关 |
| v1.0.13 | 🟡 | — | `setgroups` / `uid_map` 写入放行 |
| v1.0.12 | 🟡 | — | relabel 脚本重写为 toybox 兼容 |
| v1.0.11 | ⚪ | — | relabel 用 `find -exec {} +`，Android 不支持 |
| v1.0.10 | ⚪ | — | 本项目基线 |

---

## v1.0.20 (2026-10-28)

只改外围，**内核模块（ko）未改动**，md5 与 v1.0.19 完全相同：
`15fc08cbd67c06c3aa079bedd75b51bc`

动因：v1.0.19 的状态页一边显示「容器 uid_map 未映射」，一边显示「真 userns 已生效 · 18/18 真映射」，
自己跟自己打架。更危险的是「转换容器属主」按钮仍可点 —— 点一次就是 52 万文件改成 200000，
容器立刻 `su: cannot set groups`，再花 132 秒 restore。这个循环已经发生过三次。

- **WebUI**：真 userns 状态改为按容器进程的真实 `uid_map` 判定。
  `0 0 4294967295`（宿主恒等全映射）= 未生效，不再拿模块自报的 `real=1` 当"已生效"。
- **WebUI**：探针文案 `18/18 · 真映射` → `18/18 · 钩子已挂`，避免暗示隔离成立。
- **WebUI**：容器不在 user namespace 时禁用「转换容器属主」，`doRelabel()` 里再拦一次。
- **WebUI**：移除"自动补写 `real_userns` 意图文件并提示已生效"的逻辑。
- **`userns.sh`**：`relabel` 增加硬护栏 —— 没有任何进程在 userns 里（`uid_map` 全是
  宿主恒等映射）时拒绝执行，除非显式 `--force`。
- **README / 文档**：把"真 userns 可选开启"整节改写为"本机不支持"，补四条证据链；
  修正 `docs/PRINCIPLE.md` 里"进入真实 uid 空间 / 降权是真的"这类与事实相反的表述；
  英文文档版本号从 `v1.0.4` 统一到 `v1.0.20`。
- **`post-fs-data.sh`**：删除仍在传 `userns_adopt=1` 的逻辑（v1.0.19 已从 ko 删掉该路径，
  脚本却还在传，等于留了一颗埋着的雷）。
- 版本 `v1.0.20-6.12` / `versionCode=10020`。

## v1.0.19 (2026-10-28)

内核模块改动，**修复加载后重启**。

- `unshare` / `clone` 的 `CLONE_NEWUSER` 一律"假成功"：剥掉该 flag，其余命名空间位
  照常交给内核；不再改写 cred 的 `user_ns`，不再伪造 `struct user_namespace`。
- 删除 v1.0.18 引入的 `kernel_clone` / `do_execve` 换凭据路径。
  该路径把伪造的 `user_ns` 塞进 cred，而 `cap_capable()` 里遍历
  `cred->user_ns->parent` 的代码**没有** `#ifdef CONFIG_USER_NS`，会读到野指针。
- ko md5：`15fc08cbd67c06c3aa079bedd75b51bc`

**真机验证**：

```
unshare -U true              → rc=0（以前 126 Permission denied）
dmesg                        → unshare: ... flags=0x10000000 -> 0x0 (flag dropped)
容器内 id                    → uid=0(root) gid=0(root)
容器内 ls -ln /              → 属主全 0 0
容器内 su -c id              → uid=0(root)（不再 cannot set groups）
```

## v1.0.18 (2026-10-28) 🔴

**这一版会重启手机，请勿使用。** 保留仅作记录。

- 新增 `kernel_clone` 钩子：`needs_adopt()` 为真时临时换 cred 再 clone，让子进程带上容器 id。
- 新增 `do_execve` 钩子作为兜底。
- 问题：`kernel_clone` 是**每次 fork 都走**的全局热路径。每换一次 `user_ns`，
  `commit_creds()` 就要 `put_user_ns()` 一次，而 `cap_capable()` 遍历
  `cred->user_ns->parent` 时读到 kmalloc 伪造结构的野指针 → 硬件看门狗接管 → 整机重启。
- pstore 里没有 oops 栈，只有联发科 CLKCHK + mrdump，属于"引用计数/指针被打穿后直接 hang"。

## v1.0.17 (2026-10-28) 🟠

- 发现 Droidspaces 走的是 `unshare(CLONE_NEWNS)`（`flags=0x20000`），不是 fork。
- 改为在 `unshare` 内临时换 cred → 建 mnt ns → 还原凭据。
- 真机结果：`adopt: unshare-newns ... built ns base=200000 flags=0x20000` ✅
- **但后续子进程（`ds-monitor` / `md-udevd` / `sh` / `d-logind`）euid 仍是 0**，
  凭据没有传给后代 —— 只成功了一半。

## v1.0.16 (2026-10-28) ⚪

- 尝试在 `fork` / `copy_creds` 抢命名空间。
- 从未触发：Droidspaces 不用 fork 建容器，用 `unshare`。

## v1.0.15 (2026-10-28) 🟡

修复两次关机中的第二次。

- 根因：`vfs_open → try_module_get → do_page_fault`（Comm: `head`）——
  模块被 `rmmod` 时，`file_system_type` / ns 对象仍被引用 → UAF。
- 加硬保护：
  - `droid_lkm_userns_busy()`：refcount>1 时 `release` 返回 `-EBUSY`
  - devtmpfs 检查 `s_active > 1` 时拒绝卸载
  - `unregister` 失败也返回 `-EBUSY`
- 新增 `dsm/tools/fake_config.sh`：把 `/proc/config.gz` 里的 `CONFIG_USER_NS`
  改成 `=y` 再 bind mount 覆盖，让 App 的能力探测过关。
  **这个脚本后来成了最大的误导源**（见下方"复盘"）。

## v1.0.14 (2026-10-28) 🟡

- App 把 `allow_sandboxing` 写回 0：非 root 的 `unshare` 被 `-EINVAL` 拦。
- 加 `userns_any=1`：普通应用请求 `CLONE_NEWUSER` 时"假成功"（剥 flag、不建 ns、不动 cred）。
  只为让能力探测过关。

## v1.0.13 (2026-10-28) 🟡

- `setgroups_write` 由恒 `-EPERM` 改为接受 `deny` 并置 `USERNS_SETGROUPS_ALLOWED`。
- `map_write` 由只收 identity 映射改为**一律返回成功但不覆盖**已建映射。
- 目的：让 App 的标准 userns 四步流程（`unshare` → `deny` → `uid_map` → `gid_map`）走得通。

## v1.0.12 (2026-10-28) 🟡

`userns.sh` 重写为 Android（toybox）兼容。

两个坑：
1. toybox `find` **不支持** `-exec ... {} +` → 改 `-print0 | xargs -0`
2. toybox `find` **不支持** `-uid`（`find: bad arg`）→ 建表法：

```sh
find "$ROOT" -print0 | xargs -0 stat -c '%u %n' > /tmp/ids
cut -d' ' -f1 /tmp/ids | sort -un            # 取出实际存在的 uid
sed -n "s/^$id //p" /tmp/ids | tr '\n' '\0' | xargs -0 chown -h "$target"
```

坑中坑：`xargs -r` 在空输入时返回 **0**，脚本会打印 36 个假 `ok` 却一个文件都没改。

真机实测：52.3 万文件，relabel **174 秒**，restore **132 秒**，`check` 归零。

## v1.0.11 (2026-10-28) ⚪

- relabel 脚本用 `find -exec {} +`，在 Android 上**静默无效**（toybox 不支持该形式）。

## v1.0.10 (2026-10-28) ⚪

- 本项目基线版本。

---

## 复盘：为什么绕了三天

这一节写给以后接手的人（包括未来的自己）。

### 错误链

| 步骤 | 当时的判断 | 实际 |
|---|---|---|
| 1 | `/proc/sys/user/max_user_namespaces = 62497` → 内核支持 userns | 那只是厂商内核里的**残留节点**，值是 `pid_max` 派生的 |
| 2 | App 的 `unshare` 探测失败 → App 不会用，我替它造 | App 在**请求真命名空间**，是模块回了 `-EINVAL` 把它挡回去 |
| 3 | 造一个 `struct user_namespace` 就能骗过 `cap_capable()` | `cap_capable()` 遍历 `->parent` 链时读到野指针 |
| 4 | `fake_config.sh` 把 `config.gz` 改成 `=y`，让 App 高兴 | **骗 App 的同时把自己也骗了**，之后所有判断都建立在假前提上 |
| 5 | 18/18 钩子挂上、dmesg 有 `userns hooks installed` → 功能在工作 | 钩子是挂上了，但 `droid_lkm_userns_of()` 找不到 ns 就直通，翻译恒等 |

### 正确的第一步

```sh
su -c 'echo 1 > /sys/module/droid_lkm_misc/parameters/release
       rmmod droid_lkm_misc
       unshare -U true; echo "rc=$?"'
```

`Invalid argument`（EINVAL）= 内核没编译 `CONFIG_USER_NS`。
**这一条两秒钟，能在第一天结束全部弯路。**

再加上 `/proc/config.gz` 里那行 `# CONFIG_USER_NS is not set` —— 它一直是真话。

### 代价

- 六版代码（v1.0.13 ~ v1.0.18）
- 两次整机重启
- 三次 relabel/restore（52 万文件 × 3，累计约 8 分钟设备时间）
- 用户的时间

### 结论

**userns 不是"一个结构体 + 一张表"，是 capability 的作用域分层。**
它在 `kernel/user_namespace.c` 里，而 `CONFIG_USER_NS=n` 时那个文件根本没被编译。
模块能 hook 已存在的代码，变不出不存在的函数。

---

## 构建

```sh
sh src/scripts/build-ddkk.sh android16-6.12     # 编译 ko
sh src/scripts/package-ksu-zip.sh out.zip       # 打 KernelSU 模块包
```

编译产物目录 `droid_lkm/out/` 与 `dist/` 未包含在本仓库内（见 `.gitignore`）。
