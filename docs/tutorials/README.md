# 教程

面向"我想把它用起来"的人。每篇都能独立看，命令可以整段复制。

> 前提（第 1~6 篇）：手机是 **一加 Ace 6 至尊版（PMB110）**，内核 `6.12.69-*`，已装 **KernelSU** 和 **Droidspaces App**。
> 别的机型不要照抄，模块自带机型锁，装了也不会加载 —— 要移植到自己的手机，看[第 7 篇](07-build-your-own.md)。

## 学习路径

```
① 安装 ──→ ② 第一个容器 ──→ ③ 装软件
                              │
                              ├──→ ④ Docker
                              ├──→ ⑤ 传文件 / 备份
                              │
                              └──→ ⑥ 升级 / 回退 / 卸载

⑦ 自己构建 / 适配你的手机（换机型时看，独立于 ①~⑥）
```

| # | 教程 | 讲什么 | 时间 |
|---|---|---|---|
| 1 | [安装与首次验证](01-install.md) | 刷模块、点「加载」、确认六件事 | 10 分钟 |
| 2 | [第一个容器](02-container.md) | 创建、启动、进入、配置项逐条解释 | 15 分钟 |
| 3 | [装软件](03-software.md) | apt、nginx、redis、python、服务自启 | 20 分钟 |
| 4 | [容器里跑 Docker](04-docker.md) | dockerd、镜像、端口、数据目录 | 20 分钟 |
| 5 | [传文件与备份](05-files.md) | 宿主↔容器、打包、迁移、恢复 | 15 分钟 |
| 6 | [升级 / 回退 / 卸载](06-maintenance.md) | 换版本、安全卸载、别踩的坑 | 10 分钟 |
| 7 | **[自己构建 / 适配你的手机](07-build-your-own.md)** | 换机型换内核，怎么编出能 insmod 的 ko | 40 分钟 |

## 三句话先说清楚

**① 这个容器不是沙箱。**

容器 root **就是**宿主 root（内核没有 `CONFIG_USER_NS`，做不到隔离），32 个 capability 全量。
它和你在 Termux 里 `su` 之后跑程序是同一权限级别，只是把文件系统圈起来了。

→ 自己写的服务、`apt` 装的软件：放心跑。
→ 来路不明的镜像、`curl xxx.sh | bash`：等于把手机交给它。

细节见 [仓库 README](../../README.md#安全模型说清楚不粉饰)。

**② 不要点「转换容器属主」。**

那个按钮会把 rootfs 里 52 万个文件改成 uid 200000，容器立刻坏掉（`su: cannot set groups`），
然后要花 132 秒 restore 才能回来。v1.0.20 已经把它禁用了，但**别去绕过**。

原因见 [CHANGELOG](../../CHANGELOG.md#复盘为什么绕了三天)。

**③ 出问题先看 [TROUBLESHOOTING](../TROUBLESHOOTING.md)**，9 类故障按症状索引，比乱试快。

## 常用命令速查

```sh
# 看模块在不在
su -c 'grep "^droid_lkm" /proc/modules'

# 手动加载（开机自启关着时用）
su -c 'PH_ACTION=1 sh /data/adb/modules/Droidspaces/post-fs-data.sh'

# 启动 / 停止 / 进入容器
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config start'
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'
su -c '/data/local/Droidspaces/bin/droidspaces --name=root enter'

# 找容器进程
su -c 'P=$(grep -lF "Containers/root/rootfs" /proc/[0-9]*/mountinfo 2>/dev/null | head -1 | cut -d/ -f3); echo $P'
```
