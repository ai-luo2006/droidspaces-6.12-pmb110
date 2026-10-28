# ③ 在容器里装软件

目标：`apt` 能用、能装 nginx / redis / python、服务能自启、能从手机或局域网访问。

---

## 1. 先更新源

```sh
apt update
apt upgrade -y
```

如果 `apt update` 报 GPG 或 404，先看系统版本对不对：

```sh
cat /etc/os-release
```

Debian 12 / Ubuntu 22.04 都是长期支持的，源一般没问题。

## 2. 基本工具

```sh
apt install -y curl wget git vim htop net-tools iputils-ping unzip sudo
```

`sudo` 装不装都行 —— 你本来就在 root 下。

## 3. nginx

```sh
apt install -y nginx
systemctl enable --now nginx
systemctl status nginx --no-pager
curl -I 127.0.0.1
```

**从手机访问**：

`net_mode=host` 意味着容器直接用宿主网络栈，**不需要端口映射**。
手机浏览器打开 `http://127.0.0.1` 或 `http://<手机IP>` 就能看到 nginx 欢迎页。

**从局域网访问**：直接访问手机的局域网 IP（`ip -4 addr` 里 `wlan0` 那个），端口 80。

> ⚠️ 这也意味着**没有任何网络隔离**。手机连着的 Wi-Fi 里，别人能扫到你的 80 端口。
> 不想被扫就绑 `127.0.0.1`，见第 6 节。

## 4. redis

```sh
apt install -y redis-server
systemctl enable --now redis-server
redis-cli ping          # 期望 PONG
```

默认监听 `127.0.0.1:6379`，只有容器内能连。要给别的服务用，改 `/etc/redis/redis.conf` 里的 `bind`。

**永远不要把 redis 暴露到 0.0.0.0 且不设密码** —— 这是被挖矿最常见的方式。

## 5. python

```sh
apt install -y python3 python3-pip python3-venv
python3 -V
pip3 -V
```

建虚拟环境（推荐，别直接 `pip install` 到系统里）：

```sh
python3 -m venv /opt/myapp/venv
source /opt/myapp/venv/bin/activate
pip install flask requests
```

`pip` 慢的话换源：

```sh
pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple
```

## 6. 服务自启与监听地址

容器 init 是 systemd，所以 `systemctl enable` 就是"随容器启动自启"。

```sh
systemctl enable nginx          # 容器启动时自动起
systemctl disable nginx         # 取消
systemctl list-unit-files --state=enabled
```

**只让本机访问**（推荐，尤其是不需要对外提供服务时）：

```sh
# nginx：改 listen 行
sed -i 's/listen 80 default_server;/listen 127.0.0.1:80 default_server;/' /etc/nginx/sites-enabled/default
nginx -t && systemctl reload nginx
```

其它服务同理，找 `bind` / `listen` 配置改成 `127.0.0.1`。

## 7. 端口冲突：容器和宿主抢同一个端口

因为 `net_mode=host`，容器里的 80 端口就是宿主机的 80 端口。
如果宿主上已经有服务占了，容器里起不来，报 `Address already in use`。

查是谁占的：

```sh
# 在 Termux（宿主侧）跑
su -c 'ss -lntp | grep :80'
```

处理：把容器里的服务换个端口，或者停掉宿主那个。

## 8. 容器里的用户（别全用 root）

跑对外服务建议建普通用户：

```sh
useradd -m -s /bin/bash app
mkdir -p /opt/myapp && chown -R app:app /opt/myapp

# 用 systemd 以 app 身份跑
cat > /etc/systemd/system/myapp.service <<'EOF'
[Unit]
Description=My App
After=network.target

[Service]
User=app
WorkingDirectory=/opt/myapp
ExecStart=/opt/myapp/venv/bin/python main.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now myapp
journalctl -u myapp -f
```

> 注意：这只是**容器内的权限分离**，不是安全边界。
> 容器 root 仍是宿主 root，`app` 用户能做的事受容器内 DAC 限制，但容器 root 不受。

## 9. 磁盘占用怎么看

```sh
df -h
```

⚠️ 这里显示的 `/` 是**宿主 `/data` 分区**的整体使用量（通常 200G+），
不是容器自己占的空间。容器实际占用：

```sh
du -sh /var/lib/docker /usr /opt 2>/dev/null
```

## 10. 装崩了怎么办

容器是独立文件系统，装坏了不影响手机。三条恢复路：

```sh
# A. 只删单个包
apt remove --purge <包名> && apt autoremove -y

# B. 快照（先做，再做危险操作）
#    见 ⑤ 传文件与备份

# C. 重建容器（最彻底）
#    App 里删容器 → 重新创建 → 重新装
```

---

下一步 → [④ 容器里跑 Docker](04-docker.md)
