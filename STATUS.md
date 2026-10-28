# 真机实测状态

> 机型：一加 Ace 6 至尊版 **PMB110**
> 内核：`6.12.69-android16-6-g242e1a07f878-ab15865446-4k`
> 系统：`PMB110_17.0.0.101(CN01)`
> 记录时间：2026-10-28

本文记录的是**这台机器上跑出来的真实数字**，不是设计意图。

---

## 一、模块加载

```sh
$ grep "^droid_lkm" /proc/modules
droid_lkm_pids    0
droid_lkm_vendor  0
droid_lkm          2
droid_lkm_misc     1
```

| 模块 | 状态 | 说明 |
|---|---|---|
| `droid_lkm` | ✅ 已加载（refcount 2） | PID / IPC / mqueue 命名空间 |
| `droid_lkm_vendor` | ✅ 已加载 | oplus 厂商兼容层 |
| `droid_lkm_misc` | ✅ 已加载（refcount 1） | devtmpfs + userns 钩子（钩子空转） |
| `droid_lkm_pids` | ✅ 已加载 | pids 控制器 |
| `macvlan` | ✅ 已加载 | 容器独立 IP |
| `ip6table_nat` | ✅ 已加载 | IPv6 NAT |

WebUI 显示「已加载 6/6」。

> ⚠️ WebUI 顶部那行是 `desc.sh` 写进 `module.prop` 的**文字**，不是实时状态。
> 判断实时状态要看 `/proc/modules`。

## 二、命名空间

以容器 init 进程（`comm=systemd`）为例：

```sh
$ ls -l /proc/<pid>/ns/
cgroup    -> cgroup:[4026538664]
mnt       -> mnt:[4026538665]
net       -> net:[4026531840]
time      -> time:[4026531834]
time_for_children -> time:[4026531834]
uts       -> uts:[4026538656]
```

| ns | 容器 | 宿主（PID 1） | 结论 |
|---|---|---|---|
| pid | `4026538661` | `4026531836` | ✅ **独立** |
| mnt | `4026538665` | `4026531841` | ✅ **独立** |
| uts | `4026538656` | — | ✅ 独立 |
| cgroup | `4026538664` | — | ✅ 独立 |
| net | `4026531840` | 同值 | ⚠️ `net_mode=host`，共用宿主网络栈（预期行为） |
| user | — | — | ❌ **文件不存在**（内核未编译 `CONFIG_USER_NS`） |
| ipc | 未列出 | — | ⚠️ `droid_lkm` 的 IPC_NS 没落到这个容器上 |

**pid / mnt 独立是关键**：容器看不到宿主进程，`/proc/1/root` 指向容器自己的 systemd。

## 三、user namespace（结案：不可用）

四条独立证据：

| # | 观测 | 命令 | 对应 |
|---|---|---|---|
| 1 | `unshare: Invalid argument`（rc=1） | 模块卸载后 `unshare -U true` | `ksys_unshare()` 里 `#ifndef CONFIG_USER_NS return -EINVAL` |
| 2 | `# CONFIG_USER_NS is not set` | `zcat /proc/config.gz \| grep USER_NS` | 编译期就没有 |
| 3 | `0 0 4294967295` | `cat /proc/<pid>/uid_map` | 恒等全映射 = 不在任何 userns |
| 4 | 文件不存在 | `ls -l /proc/<pid>/ns/` | 没有 user 项 |

**反例（唯一一个）**：

```
/proc/sys/user/max_user_namespaces = 62497
```

这个值**不能**作为"支持 userns"的证据。它是厂商内核里的残留节点，值由 `pid_max` 派生。
本项目早期被它误导了三天。

## 四、容器 capability

```sh
$ grep -E "^Cap(Eff|Bnd|Prm)" /proc/<pid>/status
CapPrm: 000001c4ffecfffb
CapEff: 000001c4ffecfffb
CapBnd: 000001c4ffecfffb
```

解析后共 **32 个 capability 全部启用**，包括：

| Cap | bit | 后果 |
|---|---|---|
| `CAP_SYS_ADMIN` | 21 | 能 `mount` |
| `CAP_MKNOD` | 27 | 能自己造设备节点 |
| `CAP_SYS_PTRACE` | 19 | 能读写其他进程内存 |
| `CAP_SYS_MODULE` | 16 | 能加载内核模块 |
| `CAP_NET_ADMIN` | 12 | 能改网络配置 |
| `CAP_BPF` | 39 | 能加载 BPF |

`container.config` 里**没有任何 capability 控制字段**（无 `cap_drop` / `privileged` / `security_opt`），
是固定特权模型，配置改不了。

组合起来：`/dev/block` 是空的，但 `MKNOD` + `SYS_ADMIN` 可以自己建节点再 mount。
major:minor 从 `/proc/partitions` 就能读到（该文件不受 pid ns 隔离）。

**结论：容器挡误操作，不挡有意为之。**

## 五、文件系统

### 挂载（`/proc/<pid>/mountinfo`）

```
/
/tmp
/var/lib/docker      → 宿主 /data/local/Droidspaces/Containers/root/rootfs/var/lib/docker
```

`/storage/emulated/0` **已移除**（`enable_android_storage=0`）。

修改前它是这样的：

```
/0  /storage/emulated/0  rw,lazytime,user_id=0,group_id=0,allow_other
```

源内路径 `/0`、FUSE 参数 `user_id=0` —— 宿主内部存储直通。容器 root = 宿主 root，
意味着容器里任何进程都能删改手机上的全部用户文件（相册、微信、下载）。

### 属主

```sh
$ ls -ln /data/local/Droidspaces/Containers/root/rootfs
drwxr-xr-x 22 0 0 3452 rootfs
```

属主 `0 0`，与容器进程 uid 对齐 ✓

### setuid 位

```sh
$ ls -ln rootfs/usr/bin/{su,passwd,sudo}
-rwsr-xr-x 1 0 0  55544  passwd
-rwsr-xr-x 1 0 0  47264  su
-rwsr-xr-x 1 0 0 220040  sudo
```

`-rwsr-xr-x`，setuid 位在数百次 `chown` 之后仍然完好 ✓

## 六、容器内实际可用性

```sh
root@root:~# id
uid=0(root) gid=0(root) groups=0(root),5(tty),44(video),105(input),
                         786(droidspaces-gpu),3003(aid_inet),3004(aid_net_raw)

root@root:~# ls -ln /
lrwxrwxrwx.    1 0 0    7 bin -> usr/bin
drwxr-xr-x.    2 0 0 3452 boot

root@root:~# su -c id
uid=0(root) gid=0(root) ...
```

| 项 | 结果 |
|---|---|
| `id` | ✅ `uid=0(root)` |
| `ls -ln /` | ✅ 属主全 `0 0` |
| `su -c id` | ✅ 正常（历史问题 `cannot set groups` 已解决） |
| `apt-get` | ✅ 可用 |
| systemd | ✅ 正常（容器 init 就是 systemd） |
| 宝塔面板 | ✅ 能跑起来 |

## 七、容器配置（`container.config`）

```ini
name=root
hostname=root
rootfs_path=/data/local/Droidspaces/Containers/root/rootfs
net_mode=host                 # 直接用宿主网络栈
disable_ipv6=0
enable_android_storage=0      # ⚙️ 已关闭（原为 1）
enable_hw_access=0            # 设备节点收着
enable_gpu_mode=1
enable_x11=0
enable_virgl=0
enable_pulseaudio=0
selinux_permissive=1          # 容器内 SELinux 只记日志
allow_sandboxing=1            # 靠模块的 unshare 假成功才显示 enabled
volatile_mode=0
run_at_boot=0                 # ⚙️ 已关闭（原为 1，与模块的 no_boot 冲突）
enable_anland=0
force_cgroupv1=0
use_sparse_image=0
memory_limit=7918845952       # 7.4 GB
allow_swap=0
cpu_quota=400000              # 4 核
cpu_period=100000
pids_limit=1024
uuid=26eb8bb259aa2089b217c97e2fa6939f
```

## 八、已知边界

| 边界 | 原因 | 能不能改 |
|---|---|---|
| 容器 root = 宿主 root | 无 userns | ❌ 内核没编译，改不了 |
| 32 个 capability 全量 | droidspaces 固定特权模型 | ❌ config 里没这个字段 |
| 网络不隔离 | `net_mode=host` | ⚠️ 改 bridge 要 macvlan 配合，易搞成没网 |
| SELinux 不拦 | `selinux_permissive=1` | ⚠️ 关掉容器内很多操作会失败 |
| 共享存储已关 | `enable_android_storage=0` | ✅ 已处理 |
| 开机不自启 | 模块 `no_boot` + config `run_at_boot=0` | ⚙️ 需要时手动加载 |

## 九、历史故障与解决

| 故障 | 根因 | 解决 |
|---|---|---|
| 两次整机重启 | v1.0.18 `kernel_clone` 钩子在 fork 热路径换 cred | v1.0.19 删掉整条路径 |
| `vfs_open → try_module_get` panic | 容器在跑时 `rmmod`，ns 对象仍被引用 | v1.0.15 加 `-EBUSY` 检查 |
| `su: cannot set groups` | rootfs 属主 200000，但容器进程是 0 | `userns.sh restore`（132 秒） |
| relabel 卡死 / 36 个假 ok | toybox `find` 不支持 `-exec {} +` 和 `-uid` | 重写为 stat 建表法 |
| App「允许沙箱」不可用 | App 读 `config.gz` 判 `CONFIG_USER_NS=n` | `fake_config.sh`（代价：骗自己） |
| App 把 `allow_sandboxing` 写回 0 | 非 root `unshare` 被 `-EINVAL` 拦 | v1.0.14 假成功分支 |
| 每次开机白失败一次 | config `run_at_boot=1` 但模块 `no_boot` | 两边都关掉 |

详细的排查过程见 [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)。
