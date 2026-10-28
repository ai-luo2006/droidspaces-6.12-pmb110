# Droidspaces 6.12 for OnePlus PMB110

在**原厂 GKI 内核**（不刷内核、不改 App 源码）上补齐 Docker / 容器运行时缺失的内核能力的 KernelSU 模块合集。

目标机型：**一加 Ace 6 至尊版（PMB110）**，内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`

```
当前版本   v1.0.20-6.12
状态       ✅ 稳定可用（真机验证通过）
最新内核   droid_lkm_misc.ko  md5 15fc08cbd67c06c3aa079bedd75b51bc
真机结果   容器可启动 · su 正常 · apt 可用 · 不崩溃
```

---

## 一分钟了解

这个仓库做三件事：

| 能力 | 做法 | 状态 |
|---|---|---|
| **PID / IPC / mqueue 命名空间** | `droid_lkm.ko` 运行时补 `CONFIG_PID_NS` / `CONFIG_IPC_NS` / `CONFIG_SYSVIPC` / `CONFIG_POSIX_MQUEUE` | ✅ 可用 |
| **devtmpfs** | `droid_lkm_misc.ko` 注册一个文件系统，给容器 `/dev` 提供设备节点 | ✅ 可用 |
| **pids 控制器** | `droid_lkm_pids.ko`，让 `--pids-limit` 真正生效 | ✅ 可用 |
| **macvlan / IPv6 NAT** | 独立模块，容器独立 IP | ✅ 可用 |
| **user namespace（容器独立 uid）** | ~~`droid_lkm_misc.ko` 内伪造 userns~~ | ❌ **做不到，已结案** |

---

## ⚠️ 必读：`CONFIG_USER_NS` 补不了

这是整个项目最重要的一条结论，**先看这里能省几天时间**。

本机内核**编译时就没有 `CONFIG_USER_NS`**，模块无法在运行时补出来。四条互相印证的证据：

| # | 证据 | 观察方式 |
|---|---|---|
| 1 | `unshare -U true` 返回 **EINVAL**（不是 EPERM） | 卸掉模块后执行，对应 `ksys_unshare()` 里 `#ifndef CONFIG_USER_NS return -EINVAL` |
| 2 | `/proc/config.gz` 里就是 `# CONFIG_USER_NS is not set` | `zcat /proc/config.gz \| grep USER_NS` |
| 3 | 容器进程 `uid_map` = `0 0 4294967295`（恒等映射） | `cat /proc/<pid>/uid_map` |
| 4 | `/proc/<pid>/ns/user` **文件不存在** | `ls -l /proc/<pid>/ns/` |

> **`/proc/sys/user/max_user_namespaces` 有值（62497）不代表支持 userns。** 那是厂商内核里的残留节点，
> 值是 `pid_max` 派生出来的。本项目早期被它误导了整整三天，为此写坏了六版代码、让手机重启两次。

**后果**：容器 root **就是**宿主 root，不存在"容器里的 uid 0 在宿主上是 200000"这回事。
`droid_lkm_misc.ko` 里那套 18 个 id 翻译钩子挂上了，但**翻译是恒等的**，不产生任何隔离。

详见 [`docs/PRINCIPLE.md`](docs/PRINCIPLE.md) 与 [`CHANGELOG.md`](CHANGELOG.md) 末尾的复盘。

---

## 目录结构

```
.
├── README.md                  ← 你在这里
├── STATUS.md                  ← 真机实测状态（这台 PMB110 的确切结论）
├── CHANGELOG.md               ← v1.0.0 → v1.0.20 演变，每版标注可用性
├── release/
│   ├── v1.0.20/               🟢 当前版本，直接装这个
│   │   ├── Droidspaces-v1.0.20-6.12.zip   完整 KernelSU 模块包
│   │   ├── droid_lkm_misc.ko              单独的内核模块（快速换）
│   │   ├── webui-index.html               单独换 WebUI 页面
│   │   ├── userns.sh                      属主转换脚本（含护栏）
│   │   ├── fake_config.sh                 伪装 config.gz（见下方警告）
│   │   └── Droidspaces-6.12-src-v1.0.20.zip
│   └── v1.0.19/               🟡 上一版，可回退
├── archive/                   ⚪ 历史包 v1.0.10 ~ v1.0.18（含 🔴 会崩的版本）
├── assets/app-ui/             🖼 App 界面截图（README「App 界面全解」用的）
├── src/                       📦 完整源码树
│   ├── droid_lkm/             内核模块 C 源码
│   ├── dsm/                   KernelSU 模块脚本 + WebUI
│   ├── dsm-pids/              pids 子模块
│   ├── net_macvlan/ net_ip6nat/
│   └── scripts/               构建与打包脚本
└── docs/                      📖 文档
    ├── tutorials/             ⭐ 教程（从零到跑起来，6 篇）
    ├── PRINCIPLE.md           原理（中）
    ├── en/                    英文版
    ├── TROUBLESHOOTING.md     ⭐ 踩坑记录，出问题先看这个
    └── HISTORY-userns-attempts-DEPRECATED.txt   失败路线的历史记录
```

---

## 教程：从零到跑起来

不想看原理、只想把东西用起来，直接进教程。每篇独立可看，命令能整段复制。

| # | 教程 | 讲什么 | 时间 |
|---|---|---|---|
| 1 | [安装与首次验证](docs/tutorials/01-install.md) | 刷模块、点「加载」、确认六件事 | 10 分钟 |
| 2 | [第一个容器](docs/tutorials/02-container.md) | 创建、启动、进入、配置项逐条解释 | 15 分钟 |
| 3 | [装软件](docs/tutorials/03-software.md) | apt、nginx、redis、python、服务自启 | 20 分钟 |
| 4 | [容器里跑 Docker](docs/tutorials/04-docker.md) | dockerd、镜像、端口、数据目录 | 20 分钟 |
| 5 | [传文件与备份](docs/tutorials/05-files.md) | 宿主↔容器、打包、迁移、恢复 | 15 分钟 |
| 6 | [升级 / 回退 / 卸载](docs/tutorials/06-maintenance.md) | 换版本、安全卸载、别踩的坑 | 10 分钟 |

> 教程目录：[`docs/tutorials/`](docs/tutorials/README.md)。只有一加 Ace 6 至尊版（PMB110 / 6.12.69）适用，别的机型不要照抄。

> **App 里每个开关是什么、该开还是该关、配错了会怎样 → 见下面 [App 界面全解](#app-界面全解配图)。**

---

## App 界面全解（配图）

Droidspaces App 里那个「编辑容器：root」页面，一共 6 个分区、20 多个开关。
**大部分保持默认就行**，但有几个开关选错会让容器直接不能用，或者把手机文件暴露给容器。

下面按页面顺序过一遍，每张图都是这台 PMB110 上的实际截图（v7.1.0）。

---

### 一、启动前的自检页（`Checking system requirements`）

点「启动」时先跑一次，输出分三档：

```
[MUST HAVE]      缺一个就起不来
[RECOMMENDED]    缺了能起，但功能不全
[OPTIONAL]       只影响特定功能
```

你这台机器现在全是 ✓，但**哪些是真的、哪些是模块补的、哪些是假的**，必须分清：

| 项 | 输出 | 真相 |
|---|---|---|
| Root / Linux / Mount / UTS namespace | ✓ | 原厂就有 |
| **PID namespace** | ✓ | **原厂没有**，`droid_lkm.ko` 补的。模块没加载这项会变 ✗ |
| **IPC namespace** | ✓ | 同上，`droid_lkm.ko` 补的 |
| pivot_root / /proc / /sys / Seccomp | ✓ | 原厂就有 |
| **devtmpfs support** | ✓ | **原厂没有**，`droid_lkm_misc.ko` 注册的 |
| Cgroup v2 / Cgroup namespace | ✓ | 原厂就有 |
| **Macvlan / IPv6 NAT support** | ✓ | **原厂没有**，`net_macvlan` / `net_ip6nat` 补的 |
| **Sandboxing (user namespaces)** | ✓ | ⚠️ **假的**。内核没编译 `CONFIG_USER_NS`，是模块让 `unshare()` 返回成功骗过了探测 |

> 最后一条是这台机器最大的坑。它显示 ✓，App 日志里也会写 `Sandboxing: enabled`，
> 但**容器里没有任何 user namespace**，容器 root 就是宿主 root。
> 判定方法只有一条：看容器进程的 `/proc/<pid>/uid_map`，是不是 `0 0 4294967295`。
> 详见本文开头那节「⚠️ 必读：`CONFIG_USER_NS` 补不了」。

---

### 二、编辑容器页（6 个分区）

#### 1. 一般设置

<img src="assets/app-ui/01-general.jpg" width="420" alt="一般设置">

只有一项「主机名」。留空就用容器名（这里是 `root`），容器里 `hostname` 显示的就是它。

顶部那条红色提示 **「容器运行中 · 重启后生效」** 是最需要注意的：
**在这个页面上改任何东西，都要 `停止 → 启动` 才生效**，热改不会应用。
而且改完一定要点最底下的「保存更改」，否则退出就丢了。

#### 2. 网络

<img src="assets/app-ui/02-network.jpg" width="420" alt="网络">

| 项 | 这台机器的值 | 建议 |
|---|---|---|
| 网络模式 | **主机**（`net_mode=host`） | **保持**。容器直接用宿主网络栈，里面起的服务立刻对局域网可见。改 bridge 需要 macvlan 链路配合，配错就是「容器没网」 |
| 禁用 IPv6 | 关 | **保持关**。开了有概率让宿主上的 VPN 类 App 异常 |
| DNS 服务器 | 空 | 留空，跟随宿主。容器里 DNS 坏了才填，比如 `223.5.5.5` |

「主机」模式的代价：**没有网络隔离**。容器里的 nginx 监听 80，就是手机的 80，宿主的服务抢同一个端口会冲突。

#### 3. 集成与硬件

<img src="assets/app-ui/03-hardware.jpg" width="420" alt="集成与硬件">

| 项 | 这台机器的值 | 建议 | 为什么 |
|---|---|---|---|
| **Android 存储** | **关** | **保持关** | 开了会把 `/storage/emulated/0`（相册、下载、微信文件）挂进容器。容器 root = 宿主 root，等于全部用户文件交给容器里的任何程序 |
| 硬件访问 | 关 | **保持关** | 开了往容器 `/dev` 塞完整设备节点，配合容器里的 `CAP_MKNOD` 能碰块设备 |
| **GPU 访问** | **开** | 按需 | 把 GPU 节点**镜像**进隔离的 `/dev`，不是完整直通。要跑 GPU 相关的东西才开 |
| 配置 Termux:X11 | 灰（不可用） | 忽略 | 需要另外装 Termux 和 Termux:X11 两个 App |
| Anland 显示 | 灰（不可用） | 忽略 | 要 Anland 5.x，不支持 6.x |
| 配置 VirGL 3D 加速 | 关 | 保持关 | 软件 3D，性能不如直通，只在没有 GPU 访问时用 |
| 配置 PulseAudio | 关 | 保持关 | 容器里要出声才开，多数服务端场景用不上 |

#### 4. 资源限制

<img src="assets/app-ui/04-resources.jpg" width="420" alt="资源限制">

| 项 | 这台机器的值 | 说明 |
|---|---|---|
| 内存限制 | 开，**7.4 GB** / 上限 14.9 GB | 对应 `memory_limit`。拉到最右就是「不限」，容器吃满内存会触发系统杀进程 |
| 允许交换空间 | 关 | 开了容器可以用 swap，手机上 swap 是 zram，意义不大 |
| 限制 CPU | 开，**4 核** / 上限 8 核 | 对应 `cpu_quota=400000` / `cpu_period=100000`，即 4 个核的配额 |
| 限制进程数 | 开，**1024** | 对应 `pids_limit`。跑 Docker + systemd 建议 ≥ 1024，留空=不限 |

> 面板上看到的「内存 133MB / 7.7GB」是 cgroup 读数，**是容器自己的**；
> 但 `df` 的磁盘占用和 `/proc/loadavg` 的负载**是整台手机的**，不随命名空间隔离，别被它吓到。

#### 5. 安全与启动

<img src="assets/app-ui/05-security.jpg" width="420" alt="安全与启动">

这一块是**最容易配错**的：

| 项 | 这台机器的值 | 建议 | 说明 |
|---|---|---|---|
| SELinux 宽容模式 | **开** | **保持开** | 关了容器里很多操作会被 SELinux 拒（`avc: denied`），apt、systemd 都可能失败 |
| **允许沙箱** | **开** | **保持开** | 关了 App 会显示「允许沙箱不可用」。⚠️ 它只是让 App 把 `allow_sandboxing=1` 写进配置并做一次 `unshare` 探测，**不产生真正的 uid 隔离** |
| 易失模式 | 关 | 保持关 | 仅内存模式，停止后容器内所有改动丢失 |
| 强制 Cgroup V1 | 关 | 保持关 | 只有老镜像需要旧版层级 |
| 特权模式 | 未配置 | **别动** | 点进去才知道具体内容。实测这台机器上进程本来就有 32 个 capability，这个开关不是「有没有特权」的开关 |
| 开机自启 | **关** | **保持关**（当前） | 对应 `run_at_boot`。模块那边 `no_boot` 也是关的，两边必须对齐，否则每次开机白失败一次 |

> ⚠️ **「转换容器属主」不要在 WebUI 里点。** 那个按钮是把 rootfs 里 52 万个文件改成 uid 200000，
> 点一次容器立刻 `su: cannot set groups`，要跑 130 秒才能恢复。v1.0.20 已禁用，别绕过去。

#### 6. 高级选项

<img src="assets/app-ui/06-advanced.jpg" width="420" alt="高级选项">

页面上有两块都叫「高级选项」，第一块三项和上面「安全与启动」重复（`强制 Cgroup V1` / `特权模式` / `开机自启`），**以点进去的实际值为准**，两处是同一个字段。

| 项 | 建议 | 说明 |
|---|---|---|
| 环境变量 | 未配置 | 要传 `TZ`、`LANG`、代理才点进去加 |
| **绑定挂载点** | 未配置 | ⚠️ 这里是**往容器里挂宿主目录**的地方。挂 `/sdcard`、`/data/media` 进去 = 把手机文件交给容器，和「Android 存储」开关同等风险 |
| 指定 init 可执行文件路径 | 留空 | 留空用 `/sbin/init`（systemd）。要换成别的 init 才填 |

---

### 三、日志页：三行字判断容器状态

点「日志」弹出的窗口，**不用全看，只看 `Features` 那三行**。

#### 正常运行

<img src="assets/app-ui/07-log-running.jpg" width="420" alt="容器运行日志">

```
Container: root (RUNNING)
  PID: 32302
  OS : Ubuntu 22.04.5 LTS
  Hostname: root

Features:
  Networking: host          ← 网络模式，跟配置页一致
  HW access : GPU           ← 硬件访问，只镜像了 GPU 节点
  SELinux   : Permissive    ← 宽容模式
  Sandboxing: enabled       ← ⚠️ 只代表「允许沙箱」开着，不代表有 userns

Resources:
  Memory : 7.38 GB
  Swap   : not allowed
  CPU    : 4.00 cores
  PIDs   : 1024

[+] Container 'root' is running in background
[+] Use 'su -c "droidspaces --name='root' enter"' to connect.
```

读法：

| 行 | 正常值 | 异常时怎么办 |
|---|---|---|
| `(RUNNING)` | 运行中 | 显示 `STOPPED` = 没起来，往上翻日志找第一条报错 |
| `PID` | 一个数字 | 没有 PID = 进程已退 |
| `OS` | 你的发行版 | 显示不对 = rootfs 路径错了 |
| `Networking` | `host` | 跟你配的不一致 = 配置没生效（改完没重启） |
| `Sandboxing` | `enabled` | 显示 `disabled` = 「允许沙箱」被关了，App 会显示不可用 |
| `Memory / CPU / PIDs` | 跟配置页一致 | 不一致 = 配置没保存或没重启 |

#### 重启过程的日志

<img src="assets/app-ui/08-log-restart.jpg" width="420" alt="重启日志">

```
Starting restart operation...
[+] Restarting container root...
[+] Stopping container 'root' (PID 25120)...
[+] Waiting for graceful shutdown (this may take up to 15 seconds)...
[+] Container 'root' stopped.

Welcome to Droidspaces v7.1.0 !
[+] Configuring host-side networking for root...
[+] Applying Android system optimizations...
[+] Container started with PID 32302 (Monitor...
[+] Ensuring /data is mounted with suid supp...
[+] Setting up GPU-only access...
[+] Booting 'root' (init: /sbin/init)...
```

区分这几段：

| 段 | 含义 |
|---|---|
| `Stopping ... Waiting for graceful shutdown` | 正常停容器，最多等 15 秒。**这里卡住超过 30 秒**说明容器里有进程不肯退（常见是 docker 里的容器没停） |
| `Container 'root' stopped.` | 旧进程真的没了。**没有这一行就不要往下走**，否则新进程会和旧的抢同一个 rootfs |
| `Configuring host-side networking` | 主机网络模式在做 iptables / 路由设置 |
| `Setting up GPU-only access` | 只镜像 GPU 节点，不是完整直通 |
| `Booting 'root' (init: /sbin/init)` | 开始跑容器里的 systemd，之后就该看到 `(RUNNING)` |

**判断「这次启动成功了没」只看两行**：日志末尾有没有 `Container: root (RUNNING)` + `Command executed (exit code: 0)`。
没有就是失败，往上找第一条 `[-]`。

---

### 四、这张表存下来，配错了对照改

| 开关 | 应该 | 配错的后果 |
|---|---|---|
| Android 存储 | **关** | 容器能删你相册、微信文件 |
| 硬件访问 | **关** | 容器能碰块设备 |
| 允许沙箱 | **开** | App 显示「不可用」，Docker 起不来 |
| SELinux 宽容 | **开** | 容器内 apt/systemd 各种 permission denied |
| 开机自启 | **关**（配 `no_boot`） | 每次开机白失败一次 |
| 网络模式 | **主机** | 改 bridge 容易没网 |
| 绑定挂载点 | **空** | 等于手动把宿主目录交出去 |
| 内存 / CPU / PIDs | 按需，别拉满 | 拉满 = 容器能把手机吃死 |

改完配置的正确顺序永远是：

```sh
# 1) App 里点「保存更改」
# 2) 停容器
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'
# 3) 确认配置真的写进文件了
su -c 'grep -E "^(enable_android_storage|enable_hw_access|allow_sandboxing|run_at_boot)=" /data/local/Droidspaces/Containers/root/container.config'
# 4) 再启动
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config start 2>&1 | tail -5'
```

> App 界面有缓存：界面上看到的开关状态，**不一定**是 config 文件里的实际值。
> 遇到「界面显示开、文件里是 0」的情况，以**文件**为准；这时候在 App 里点保存会把界面状态覆盖回去。

---

---

## 安装

### 方式 A：完整包（推荐首次安装）

KernelSU → 模块 → 安装 → 选 `release/v1.0.20/Droidspaces-v1.0.20-6.12.zip` → 重启

首次安装**默认关闭开机自启**（`no_boot` 存在），需要手动点一次「加载」验证手机正常，
再回 WebUI 打开「开机自动加载」。

### 方式 B：只换内核模块（快，不用重启）

```sh
# 先把 droid_lkm_misc.ko 放到 /sdcard/Download/
su -c 'droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop
       echo 1 > /sys/module/droid_lkm_misc/parameters/release
       rmmod droid_lkm_misc
       cp /sdcard/Download/droid_lkm_misc.ko /data/adb/modules/Droidspaces/ko/
       insmod /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko real_userns=1 userns_any=1
       md5sum /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko'
```

md5 应为 `15fc08cbd67c06c3aa079bedd75b51bc`。

### 方式 C：只换 WebUI 页面（最轻）

```sh
su -c 'cp /sdcard/Download/webui-index.html /data/adb/modules/Droidspaces/webroot/index.html
       md5sum /data/adb/modules/Droidspaces/webroot/index.html'
```

md5 应为 `75f679574e0347b46eead02789b405a7`。

---

## 文件标注

### 🟢 `release/v1.0.20/` — 当前版本，用这个

| 文件 | 大小 | md5 | 用途 |
|---|---|---|---|
| `Droidspaces-v1.0.20-6.12.zip` | 7.2 MB | `fef08ae8ca10009632aee397c36d6e3c` | 完整包，KernelSU 刷入 |
| `droid_lkm_misc.ko` | 4.5 MB | `15fc08cbd67c06c3aa079bedd75b51bc` | 单独的内核模块 |
| `webui-index.html` | 75 KB | `75f679574e0347b46eead02789b405a7` | 状态页，防误点 |
| `userns.sh` | 12 KB | `4c49905b36aeeac95460730762eb42f1` | 属主脚本（relabel 已加护栏） |
| `fake_config.sh` | 1.8 KB | `2c5f89b7baed20a0544bd22064f43ade` | 见下方 ⚠️ |
| `Droidspaces-6.12-src-v1.0.20.zip` | 710 KB | `8a60aef52cd7192ba71c6a6b8a969ae1` | 源码快照 |

### 🟡 `release/v1.0.19/` — 可回退

只差 WebUI 判定逻辑，内核模块与 v1.0.20 **完全相同**（同一个 md5）。
v1.0.19 的页面会把"真 userns"显示成"已生效"、且「转换容器属主」按钮可点 —— 那是危险按钮，所以推 v1.0.20。

### 🔴 `archive/` — 历史版本，**其中三版会崩手机**

| 版本 | 状态 | 说明 |
|---|---|---|
| v1.0.18 | 🔴 **禁止使用** | `kernel_clone` 钩子在每次 fork 时换 cred → **两次整机重启** |
| v1.0.17 | 🟠 半成 | 建 ns 成功但子进程凭据未切换，容器 `su` 失败 |
| v1.0.16 | ⚪ 无效 | 挂错钩子（`fork`/`copy_creds`），Droidspaces 走的是 `unshare`，从未触发 |
| v1.0.15 | 🟡 可用 | 加卸载引用检查，防 UAF；但 userns 仍无 |
| v1.0.14 | 🟡 可用 | App 探测假成功 |
| v1.0.13 | 🟡 可用 | setgroups / uid_map 写入放行 |
| v1.0.12 | 🟡 可用 | toybox 兼容的 relabel 脚本 |
| v1.0.11 | ⚪ 旧 | relabel 脚本有 toybox 兼容问题 |
| v1.0.10 | ⚪ 旧 | 基线 |

> v1.0.0 ~ v1.0.9 与早期调试包（`droidspaces_6.12_v3` ~ `v36`）未收录，
> 那是开发过程中的临时构建，与当前结构不兼容。需要的话看 [`CHANGELOG.md`](CHANGELOG.md)。

### 校验下载的文件

```sh
md5sum -c checksums.txt --ignore-missing
```

### ⚠️ `fake_config.sh` 是双刃剑

它把 `/proc/config.gz` 里的 `CONFIG_USER_NS` / `CONFIG_DEVTMPFS` 改成 `=y` 再 bind mount 覆盖，
**纯粹是为了让 Droidspaces App 的能力探测过关**（App 读 `config.gz` 判定，不看 syscall）。

问题在于：**这也在骗使用者。** 它会让人（包括开发者本人）以为内核真的支持。
本项目为此绕了三天弯路，最后靠"卸载模块后 `unshare -U true` 看 errno"才定位。

保留它是因为「允许沙箱」显示 enabled 需要它；但它不提供任何真实能力。

---

## 真机状态（PMB110 / 6.12.69）

| 项 | 结果 |
|---|---|
| 六个模块加载 | ✅ 6/6 |
| 容器启动 / 进入 | ✅ 正常 |
| 容器内 `su` | ✅ 正常（不再 `cannot set groups`） |
| 容器内 `apt` | ✅ 可用 |
| pid namespace | ✅ 独立（与宿主不同 inode） |
| mnt namespace | ✅ 独立 |
| uts / net / cgroup / time | ✅ 独立 |
| user namespace | ❌ **不存在**（内核未编译） |
| 共享存储 `/storage/emulated/0` | ⚙️ 已关闭（`enable_android_storage=0`） |
| capability | ⚠️ 全量 32 个（含 `SYS_ADMIN`、`MKNOD`），配置改不了 |
| 开机自启 | ⚙️ 默认关闭，手动加载 |

完整实测记录见 [`STATUS.md`](STATUS.md)。

---

## 🚫 不要做的事

| 别做 | 原因 |
|---|---|
| 跑 `userns.sh relabel` / 点「转换容器属主」 | 内核没有 userns，改了就是**弄坏容器**：52 万文件改 200000 → `su: cannot set groups` → 再花 132 秒 restore |
| 装 v1.0.16 ~ v1.0.18 | 带 `kernel_clone` 换凭据的路，**会整机重启** |
| 建 `userns_adopt` 文件 | 同上，第二条崩溃路径 |
| 卸载模块前不 stop 容器 | `vfs_open → try_module_get` UAF，可能 panic |
| 相信 `max_user_namespaces` 有值 = 支持 userns | 残留节点，被骗三天 |

---

## 安全模型（说清楚，不粉饰）

```
容器 root  ==  宿主 root        （无 userns，无法改变）
capability  ==  32 个全量       （含 SYS_ADMIN / MKNOD / SYS_PTRACE）
SELinux     ==  Permissive      （容器域 droidspacesd，只记日志不拦）
文件系统    ==  独立 rootfs + 独立 pid/mnt ns  ✅
网络        ==  net_mode=host，直接用宿主网络栈
```

**这个容器挡误操作，不挡有意为之。** 它的权限等价于你在 Termux 里 `su` 之后跑的东西，
只是把文件系统圈起来了。别当沙箱用。

- ✅ 适合跑：自己写的服务、`apt` 装的 nginx/redis、自己构建的镜像
- ❌ 不适合跑：来路不明的镜像、`curl xxx.sh | bash`、把它当隔离环境存密码

---

## 构建

需要 DDK（Droidspaces Development Kit）环境：

```sh
sh src/scripts/build-ddkk.sh android16-6.12     # 编译内核模块
sh src/scripts/package-ksu-zip.sh out.zip       # 打 KernelSU 包
```

产物生成在 `src/droid_lkm/out/`。

---

## 许可

见 [`src/LICENSE`](src/LICENSE) 与 [`src/THIRD-PARTY.md`](src/THIRD-PARTY.md)。
`droid_lkm` 系列模块为独立实现；Droidspaces App 本体不在此仓库内，本项目不修改其源码。
