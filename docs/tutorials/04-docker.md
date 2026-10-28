# ④ 在容器里跑 Docker

目标：容器内 `dockerd` 起来、能拉镜像、能映射端口、数据不丢。

---

## 1. 为什么这个场景可行

Docker 需要两样内核能力，原厂内核都没编：

| 需要 | 谁补的 |
|---|---|
| PID namespace | `droid_lkm.ko` |
| IPC / mqueue | `droid_lkm.ko` |
| pids cgroup（`--pids-limit` 真正生效） | `droid_lkm_pids.ko` |

模块加载正常的话，容器里 `docker info` 不会报 namespace 相关的错。

先确认：

```sh
su -c 'grep "^droid_lkm" /proc/modules'
```

四个都在再往下走。

## 2. 装 Docker

```sh
apt update
apt install -y docker.io
systemctl enable --now docker
docker info | head -20
```

官方源版本更新，但要加 GPG key：

```sh
apt install -y ca-certificates curl gnupg
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/debian $(. /etc/os-release && echo $VERSION_CODENAME) stable" > /etc/apt/sources.list.d/docker.list
apt update && apt install -y docker-ce docker-ce-cli containerd.io
```

## 3. 拉国内镜像（网络慢的话）

`docker pull` 卡住多半是 registry 问题。配加速：

```sh
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<'EOF'
{
  "registry-mirrors": [
    "https://docker.m.daocloud.io",
    "https://dockerproxy.com"
  ],
  "log-driver": "json-file",
  "log-opts": { "max-size": "10m", "max-file": "3" }
}
EOF
systemctl restart docker
docker info | grep -A3 "Registry Mirrors"
```

镜像源经常失效，失效就换别的，或者直接 `docker pull` 走代理。

## 4. 第一个容器

```sh
docker run -d --name web -p 8080:80 nginx:alpine
docker ps
curl -I 127.0.0.1:8080
```

容器里 `docker run -p 8080:80`，因为 `net_mode=host`，**手机上的 8080 就能访问**。

## 5. 端口策略（这里最容易踩）

```
用户 → 手机 8080 → 容器内 Docker 的 8080 → web 容器的 80
```

两层端口，容易搞混：

| 层 | 谁在听 | 查看方式 |
|---|---|---|
| 手机（宿主） | dockerd 的 proxy | Termux: `su -c 'ss -lntp \| grep 8080'` |
| 容器内 | dockerd 的 proxy | 容器: `ss -lntp \| grep 8080` |
| Docker 内 | 应用 | `docker port web` |

**端口冲突**：宿主上已经有服务占 8080，`docker run -p 8080:80` 会报 `port is already allocated`。

## 6. 数据持久化

**别把数据放在容器里** —— 重装镜像就没了。

```sh
# 命名卷
docker volume create redis-data
docker run -d --name redis -v redis-data:/data redis:alpine

# 绑定到容器内目录（推荐，好备份）
mkdir -p /srv/data/redis
docker run -d --name redis -v /srv/data/redis:/data redis:alpine
```

`/srv/data/redis` 在容器 rootfs 里，宿主看得见：
`/data/local/Droidspaces/Containers/root/rootfs/srv/data/redis`

> 注意 config 里原本给 `/var/lib/docker` 单独挂了一个目录，
> 所以镜像层数据在宿主上的位置是 rootfs 的 `/var/lib/docker`。

## 7. 资源限制

`pids_limit=1024` 是**整个容器**的上限，Docker 里再跑一堆容器可能不够。
跑 Docker 建议改到 4096：

```sh
su -c 'C=/data/local/Droidspaces/Containers/root/container.config
       sed -i "s/^pids_limit=.*/pids_limit=4096/" "$C"
       grep ^pids_limit "$C"'
```

改完要 `stop` + `start` 生效。

Docker 内单容器限制：

```sh
docker run -d --memory=512m --cpus=1.5 --pids-limit=200 --name app myimage
```

## 8. 常见错误

| 报错 | 原因 | 处理 |
|---|---|---|
| `Cannot connect to the Docker daemon` | dockerd 没起 | `systemctl start docker`，`journalctl -u docker -n 50` |
| `failed to create endpoint: operation not permitted` | iptables 权限 | 见下 |
| `port is already allocated` | 宿主端口被占 | `ss -lntp` 找出来，换端口 |
| `no space left on device` | 镜像太多 | `docker system prune -af` |
| `pids limit exceeded` | 进程数超限 | 提高 `pids_limit` |

`iptables` 那条：容器里 `CAP_NET_ADMIN` 是有的，但宿主 iptables 规则可能没放行转发。先看：

```sh
sysctl net.ipv4.ip_forward
iptables -t nat -L -n | head -20
```

## 9. 清理

```sh
docker system df                    # 看占了多少
docker system prune -af             # 删所有停止的容器 / 无用镜像 / 网络
docker volume prune -f              # ⚠️ 会删没挂载的卷，数据可能丢
docker image prune -a -f
```

## 10. 一条实话

**`docker run` 来的镜像，权限等于你的容器 root，也就是宿主 root。**

它能读 `/data/data/*`（其他 App 的数据）、能改系统设置、能装内核模块。
`docker` 那层 namespace 隔离只在 Docker 自己的边界内有效，出不了这个容器。

- 自己的镜像 / 官方镜像：可以
- 别人给的 tar / 来路不明的 `docker run`：等于把手机交出去

---

下一步 → [⑤ 传文件与备份](05-files.md)
