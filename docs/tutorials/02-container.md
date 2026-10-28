# ② 第一个容器

目标：建一个能跑、能进去、`su` 正常、`apt` 能用的 Debian/Ubuntu 容器。**约 15 分钟。**

---

## 1. 用 App 创建（推荐）

命令行创建容器需要交互式下载 rootfs，App 里点几下更稳。

**Droidspaces App → 新建容器**

| 选项 | 建议 |
|---|---|
| 发行版 | Debian 12 或 Ubuntu 22.04（生态最全） |
| 容器名 | `root`（默认；后面命令都按这个名字写） |
| 共享存储 | **关**（见下面第 5 节，默认是开，建议关掉） |
| 硬件访问 | 关（需要时再开） |
| GPU 加速 | 按需，跑图形/推理才开 |
| 内存 / CPU / pids | 默认即可（7.4G / 4 核 / 1024） |

创建过程要下载几百 MB，看网速。

## 2. 启动与进入

```sh
# 启动（后台运行）
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config start'

# 进入
su -c '/data/local/Droidspaces/bin/droidspaces --name=root enter'

# 停止
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'
```

成功的标志：

```
[+] Container 'root' is running in background.
[+] Use 'su -c "droidspaces --name='root' enter"' to connect.
```

进入后提示符变成 `root@root:~#`（容器内），不是 `~ $`（Termux）。

> **注意**：`droidspaces` 不在 `PATH` 里，必须写全路径 `/data/local/Droidspaces/bin/droidspaces`。
> 而且它**没有** `status` 子命令，查状态靠 `/proc/modules` 和 `mountinfo`。

## 3. 三条命令确认容器是好的

进容器后：

```sh
id
ls -ln /
su -c id
```

| 命令 | 期望 | 不对的话 |
|---|---|---|
| `id` | `uid=0(root) gid=0(root)` | 见 [TROUBLESHOOTING ②](../TROUBLESHOOTING.md#二su-cannot-set-groups) |
| `ls -ln /` | 属主全是 `0 0` | 是 `200000` → 磁盘没恢复，跑 `userns.sh restore all` |
| `su -c id` | `uid=0(root)` | 报 `cannot set groups` → 同上 |

这三条一起过了，说明**磁盘属主和进程 uid 对齐了**，后面不会再有 `su` 的怪问题。

## 4. 配置项逐条解释

文件：`/data/local/Droidspaces/Containers/root/container.config`（21 个字段）

```sh
su -c 'cat /data/local/Droidspaces/Containers/root/container.config'
```

| 字段 | 默认 | 含义 | 建议 |
|---|---|---|---|
| `name` / `hostname` | `root` | 容器名 / 主机名 | 不动 |
| `rootfs_path` | `/data/local/Droidspaces/Containers/root/rootfs` | 根文件系统位置 | 不动 |
| `net_mode` | `host` | **直接用宿主网络栈** | 不动（改 bridge 容易没网） |
| `disable_ipv6` | `0` | 关掉 IPv6 | 不动 |
| `enable_android_storage` | `1` → **改 `0`** | 把 `/storage/emulated/0` 挂进容器 | **关**（见第 5 节） |
| `enable_hw_access` | `0` | 容器 `/dev` 塞完整设备节点 | 关，需要时开 |
| `enable_gpu_mode` | `0/1` | GPU 直通 | 跑图形/推理才开 |
| `enable_x11` / `enable_virgl` | `0` | 图形界面 | 关 |
| `enable_pulseaudio` | `0` | 声音 | 关 |
| `selinux_permissive` | `1` | 容器内 SELinux 只记日志不拦 | **保持 1**，改 0 很多操作会失败 |
| `allow_sandboxing` | `1` | 「允许沙箱」 | **保持 1**，改 0 App 会显示不可用 |
| `volatile_mode` | `0` | 重启丢弃改动 | 关（除非做实验） |
| `run_at_boot` | `1` → **改 `0`** | 开机自启容器 | 看模块是否也自启，两边要对齐 |
| `enable_anland` | `0` | 网络相关 | 不动 |
| `force_cgroupv1` | `0` | 强制 cgroup v1 | 不动 |
| `use_sparse_image` | `0` | rootfs 用稀疏镜像而非目录 | 不动 |
| `memory_limit` | `7918845952` | 内存上限（约 7.4G） | 按需 |
| `allow_swap` | `0` | 允许 swap | 关 |
| `cpu_quota` / `cpu_period` | `400000` / `100000` | CPU 配额（4 核） | 按需 |
| `pids_limit` | `1024` | 最大进程数 | 跑 Docker 建议 4096+ |
| `uuid` | 随机 | 容器标识 | **不要改** |

改法：

```sh
su -c 'C=/data/local/Droidspaces/Containers/root/container.config
       cp -a "$C" "$C.bak"
       sed -i "s/^enable_android_storage=.*/enable_android_storage=0/" "$C"
       grep -n enable_android_storage "$C"'
```

> ⚠️ **改完别在 App 里点「保存配置」** —— App 会用它界面上的缓存把文件覆盖回去。

## 5. 为什么要关「共享存储」

默认 `enable_android_storage=1` 时，容器的 `/storage/emulated/0` 是宿主内部存储的 **FUSE 直通**：

```
/0  /storage/emulated/0  rw,lazytime,user_id=0,group_id=0,allow_other
```

容器 root = 宿主 root，所以容器里任何进程（包括你 `docker run` 的镜像）都能删改：

相册 · 微信聊天记录 · 下载 · Documents · 其他 App 放在 `/sdcard` 的所有东西

如果你只在容器里跑服务、不处理手机文件，**关掉更安全**。
真要传文件用 [⑤ 传文件](05-files.md) 里的方法，按需临时开。

## 6. 容器里现在有什么

进容器 `ls -ln /` 你会看到标准 Linux 目录树：

```
bin -> usr/bin
boot
dev            ← 模块 devtmpfs 提供（enable_hw_access=0 时是空壳）
etc
home
lib -> usr/lib
opt
proc
root
run
sbin -> usr/sbin
srv
sys
tmp
usr
var            ← /var/lib/docker 是单独挂的
```

它是**真的独立文件系统**，不是 chroot 到宿主目录。`/data`、`/system` 都看不见。

## 7. 日常操作

```sh
# 进容器
su -c '/data/local/Droidspaces/bin/droidspaces --name=root enter'

# 在容器里跑一条命令（不进交互 shell）
su -c '/data/local/Droidspaces/bin/droidspaces --name=root enter -c "systemctl status"'

# 看容器进程
su -c 'P=$(grep -lF "Containers/root/rootfs" /proc/[0-9]*/mountinfo 2>/dev/null | head -1 | cut -d/ -f3)
       echo "pid=$P comm=$(cat /proc/$P/comm)"'

# 看容器的命名空间
su -c 'P=...; ls -l /proc/$P/ns/'
```

容器 init 是 **systemd**，所以容器内可以直接用 `systemctl`：

```sh
systemctl status
systemctl list-units --type=service --state=running
journalctl -xe
```

## 8. 现在可以做什么

| 想做的事 | 去哪 |
|---|---|
| 装 nginx / redis / python | [③ 装软件](03-software.md) |
| 跑 Docker 镜像 | [④ Docker](04-docker.md) |
| 和手机互传文件 | [⑤ 传文件](05-files.md) |
| 换版本 / 卸载 | [⑥ 维护](06-maintenance.md) |

---

下一步 → [③ 装软件](03-software.md)
