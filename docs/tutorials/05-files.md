# ⑤ 传文件与备份

目标：手机 ↔ 容器互传文件、给容器做快照、迁移到新机器、装坏了能恢复。

---

## 1. 关键路径先记住

容器所有东西都在这个目录下（宿主视角）：

```
/data/local/Droidspaces/Containers/root/rootfs/
```

容器里的 `/` = 宿主上的 `.../root/rootfs/`

所以从 Termux 用 `su` 可以直接读写：

```sh
# 容器里的 /opt/myapp → 宿主上这个路径
/data/local/Droidspaces/Containers/root/rootfs/opt/myapp
```

**这是最简单的方式**，不需要开共享存储、不需要 `docker cp`。

## 2. 传文件（三种方法）

### 方法 A：直接从 Termux 拷（推荐，最快）

```sh
# 手机 → 容器（/sdcard/Download/ 在 Termux 里用 ~/storage/downloads 或 /sdcard 都行）
su -c 'cp /sdcard/Download/app.tar.gz /data/local/Droidspaces/Containers/root/rootfs/opt/'

# 容器 → 手机
su -c 'cp /data/local/Droidspaces/Containers/root/rootfs/opt/result.txt /sdcard/Download/'
```

属主是 root，容器里读没问题。

### 方法 B：临时打开共享存储

```sh
su -c 'C=/data/local/Droidspaces/Containers/root/container.config
       sed -i "s/^enable_android_storage=.*/enable_android_storage=1/" "$C"
       /data/local/Droidspaces/bin/droidspaces --config=$C stop
       /data/local/Droidspaces/bin/droidspaces --config=$C start'
```

容器里 `/storage/emulated/0` 就是手机内部存储。**用完记得改回 0** —— 这是一个不做隔离的口子。

### 方法 C：Docker 容器

```sh
docker cp ./file.txt web:/usr/share/nginx/html/
docker cp web:/etc/nginx/nginx.conf ./
```

## 3. 给容器做快照（备份）

**先停容器**，否则 tar 出来的是"跑着的文件系统"，可能不一致。

```sh
# 1. 停
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'

# 2. 打包（保留属主、权限、setuid 位、xattr）
su -c 'cd /data/local/Droidspaces/Containers/root
       tar --numeric-owner -czf /sdcard/Download/rootfs-$(date +%m%d-%H%M).tar.gz rootfs 2>/dev/null
       ls -lh /sdcard/Download/rootfs-*.tar.gz | tail -1'

# 3. 起
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config start'
```

`--numeric-owner` **必须加**。不加的话 tar 会按用户名还原，容器里 UID 可能错乱。

顺手备份配置：

```sh
su -c 'cp /data/local/Droidspaces/Containers/root/container.config /sdcard/Download/container.config.bak'
```

## 4. 恢复

```sh
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'

# 现在是目录还是镜像？看 config
su -c 'grep use_sparse_image /data/local/Droidspaces/Containers/root/container.config'
```

`use_sparse_image=0`（默认）→ rootfs 是普通目录，直接解压：

```sh
su -c 'cd /data/local/Droidspaces/Containers/root
       mv rootfs rootfs.broken-$(date +%s)
       tar --numeric-owner -xzpf /sdcard/Download/rootfs-1028-1600.tar.gz
       ls -ln rootfs | head -3'
```

`ls -ln rootfs` 属主应该是 `0 0`。如果是别的数字，说明打包时没加 `--numeric-owner`。

## 5. 迁移到新手机

1. 新手机装同版本模块（`v1.0.20`），确认 `uname -r` 也是 `6.12.69-*`
2. 把 `rootfs-*.tar.gz` + `container.config.bak` 拷过去
3. 只改 config 里的 `uuid`（删掉那行，App 会重新生成）
4. 解压到 `/data/local/Droidspaces/Containers/<新名字>/rootfs`
5. App 里能看到这个容器 → 启动 → 进容器验 `id` / `ls -ln /`

## 6. 装坏了怎么快速回滚

在做危险操作前（大版本升级、装一堆依赖）先打一个快照，出问题就回第 4 节。

或者更轻的方式 —— **容器本身随便折腾**，它不会影响手机：

| 想回滚的东西 | 做法 |
|---|---|
| 单个软件 | `apt remove --purge <包>` |
| 整个容器 | 删掉 → App 重建（10 分钟） |
| 数据 | 只要 `/srv`、`/opt`、`/home` 有备份就行 |

## 7. 检查磁盘

```sh
# 宿主上（Termux）
su -c 'du -sh /data/local/Droidspaces/Containers/root/rootfs'
su -c 'df -h /data'

# 容器里
du -sh /var/lib/docker /usr /opt /srv 2>/dev/null | sort -h
```

rootfs 一般 2-5 GB（Debian + 常用软件），加 Docker 镜像会大很多。

## 8. 常见坑

| 现象 | 原因 | 处理 |
|---|---|---|
| 恢复后 `su` 报 `cannot set groups` | 解包没加 `--numeric-owner`，属主错了 | 重新用 `--numeric-owner -xzpf` 解 |
| 恢复后容器里 `Permission denied` 一堆 | 同上 | 同上 |
| tar 出来 20GB，明明只用了 5GB | 打包时容器在跑，含临时文件 | 先 stop 再打 |
| `cp` 到 `/sdcard` 失败 | 权限（FUSE 限制） | 先 `cp` 到 `/data/local/tmp/` 再挪 |
| 恢复后 Docker 起不来 | `/var/lib/docker` 属主变了 | `chown -R 0:0 rootfs/var/lib/docker` |

---

下一步 → [⑥ 升级 / 回退 / 卸载](06-maintenance.md)
