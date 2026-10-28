# 部署后完整验证清单

环境：一加 Ace 6 至尊版 `PMB110`，系统 `17.0.0.101(CNO1C10P02)`，
内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`。
记录时间：2026-10-27。

所有命令以 `su -c '...'` 或已获得 root 的 shell 执行。

---

## 1. 模块加载状态

```sh
cat /proc/modules
```

期望能看到 6 行：

```
droid_lkm        ...
droid_lkm_vendor ...
droid_lkm_misc   ...
droid_lkm_pids   ...
macvlan          ...
ip6table_nat     ...
```

dmesg 里确认没有 `Unknown symbol` / `CFI failure`：

```sh
dmesg | grep -iE 'droid_lkm|macvlan|ip6nat|Unknown symbol|CFI failure' | tail -20
```

---

## 2. namespace / IPC（droid_lkm）

```sh
ls -l /proc/self/ns/{pid,ipc,mnt,uts}
unshare --pid --fork --mount-proc /bin/sh -c 'echo PIDNS_OK; ls /proc | head -3'
```

期望：容器内 `echo $$` 是 1 / 小 pid；`/proc/self/ns/ipc` 可读。

SysV IPC：

```sh
ipcs -m
ipcs -q
```

无 `Invalid argument` 即通过。

---

## 3. user namespace（droid_lkm_misc）

```sh
readlink /proc/self/ns/user
cat /proc/self/uid_map
cat /proc/self/gid_map
```

期望：`uid_map` **非空**（`0 0 4294967295`），`gid_map` 有内容。
这是"允许沙箱"能否打开的关键。

---

## 4. devtmpfs（droid_lkm_misc）

```sh
cat /proc/filesystems | grep devtmpfs
```

期望输出：

```
nodev	devtmpfs
```

只注册、不起线程；挂载与节点填充由 Droidspaces 自己完成。

---

## 5. Docker 全流程

```sh
docker run --rm hello-world
```

期望 `hello-world` 正常输出。

后台容器 + 端口：

```sh
docker run -d --name deepseek_harness -p 8080:8080 <image>
docker ps
```

期望 `STATUS` 显示 `Up (healthy)`。

容器内 namespace：

```sh
docker exec deepseek_harness ls -l /proc/self/ns/{pid,ipc,user}
```

---

## 6. macvlan（控制面）

```sh
ip link add mvt link wlan0 type macvlan mode bridge
ip link show mvt
ip addr add 10.131.204.250/24 dev mvt
ip link set mvt up
ip link del mvt
```

期望：接口能建、能删，不报错。

**数据面实测（本机不通过，属预期）**：

```sh
ping -c 2 -I mvt 10.131.204.1
ip -s link show mvt
```

实测输出：

```
PING 10.131.204.1: 56(84) bytes of data.
From 10.131.204.250: icmp_seq=1 Destination Host Unreachable
From 10.131.204.250: icmp_seq=2 Destination Host Unreachable
2 packets transmitted, 0 received, +2 errors, 100% packet loss

RX: bytes packets errors dropped overrun mcast
    0        0       0      0       0       0
TX: bytes packets errors dropped carrier collsns
    0        0       3      0       0       0
```

**判定**：TX errors=3、RX=0、ARP 永远 FAILED —— 这是**本机 WiFi 驱动只接受单个 MAC**
导致的，不是模块 bug。USB 有线网卡下可用。host 网络模式不需要 macvlan。

---

## 7. IPv6 NAT（ip6table_nat）

```sh
ip6tables -t nat -L -n
ip6tables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE
ip6tables -t nat -L -n
ip6tables -t nat -D POSTROUTING -o wlan0 -j MASQUERADE
```

期望：表可列出、规则可加可删，无 `iptables: Table does not exist`。

---

## 8. Droidspaces App 侧

编辑容器，确认「允许沙箱」「硬件访问」都能打开，且 App 提示
`Sandboxing: enabled`、`HW access: full`。

若 App 显示"硬件访问 不可用"但 `/proc/filesystems` 已有 `devtmpfs`，
是 App 缓存，清掉：

```sh
su -c 'am force-stop com.droidspaces.app'
su -c 'sed -i -e "/name=\"host_capabilities\"/d" \
        -e "/name=\"host_capabilities_boot_id\"/d" \
        /data/data/com.droidspaces.app/shared_prefs/droidspaces_prefs.xml'
```

---

## 9. pids 控制器（`droid_lkm_pids`）

> 内核未编 `CONFIG_CGROUP_PIDS`，这是**模拟实现**。原理、记账点选择、已知偏差见
> [`docs/PIDS-ENFORCEMENT.md`](docs/PIDS-ENFORCEMENT.md)。

### 9.1 加载后的探针自检

```sh
su -c 'dmesg | grep droid_lkm_pids | tail -12'
```

期望（地址因机而异）：

```
[droid_lkm_pids_ctl] layout verified: controllers=8 (built against 8)
[droid_lkm_pids_ctl] probed cgroup_subtree_control_write at 0xffffffe20458db44
[droid_lkm_pids_ctl] probed cgroup_can_fork at 0xffffffe2043a0a50
[droid_lkm_pids_ctl] probed cgroup_post_fork at 0xffffffe2043cfcd4
[droid_lkm_pids_ctl] probed cgroup_exit at 0xffffffe2044e7068
[droid_lkm_pids_ctl] probed cgroup_rmdir at 0xffffffe2047d1f2c
[droid_lkm_pids_ctl] pids limit enforcement active (emulated pids controller)
[droid_lkm_pids] pids controller name offered to the cgroup printers
```

关键行是 `layout verified` 与 `pids limit enforcement active`。只有
`layout unverified` 或 `enforcement incomplete` 时，功能层不生效（只剩显示层）。

### 9.2 App 侧

```sh
docker run --rm --pids-limit 256 hello-world
```

期望：**不再**报 `pids.max is not available`；检查项 `Process limit support` 通过。

```sh
su -c 'ls /sys/fs/cgroup/<容器目录>/ | grep pids'
```

期望出现 `pids.max` / `pids.current` / `pids.stats`。

### 9.3 功能层是否真的在拦：看 `refused`

`refused` 是"被拦下并返回 `-EAGAIN` 的 fork 次数"。**大于 0 即功能层生效。**

```sh
su -c 'cat /sys/fs/cgroup/<容器目录>/pids.stats'
```

```
subtree=3644 strip=48 rewrite=36 fork=10530 charged=10530 refused=629 rmdir=378 layout=1
```

`layout` 必须为 `1`；`fork` 应持续增长（它是全系统 fork 计数，不是容器内的）。

### 9.4 手工 cgroup 对照实验（不依赖容器）

```sh
su -c 'CG=/sys/fs/cgroup/t2; rmdir $CG 2>/dev/null; mkdir -p $CG; echo 5 > $CG/pids.max; echo $$ > $CG/cgroup.procs; read S < $CG/pids.stats; echo "start   $S"; i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); read C < $CG/pids.current; read S < $CG/pids.stats; echo "第${i}次 current=$C  $S"; done; rmdir $CG 2>/dev/null'
```

实测（`PMB110` / `6.12.69`）：

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

判定：

- `current` 涨到 4 就上不去 → 配额用尽；
- `refused` 在第 4→5 次之间 **+5** → 确有 fork 被 `-EAGAIN` 拒掉；
- 第 4→5 次之间约 6 秒停顿**不是 hang**：`/system/bin/sh` 在 `fork()` 返回
  `EAGAIN` 时会等待后重试，等第一批 `sleep 6` 到期释放配额才继续，
  于是 `current` 掉回 1 重新涨。

### 9.5 稳定性

```sh
su -c 'dmesg | grep -iE "droid_lkm_pids|Unable|panic|BUG:" | tail -20'
```

期望无 `Unable to handle kernel paging request` / `panic` / `BUG:` / `CFI failure`。

---

## 10. 结果汇总

| 项 | 判定 |
| --- | --- |
| 5 个模块加载 | ✅ |
| PID/IPC namespace | ✅ |
| SysV IPC / POSIX mqueue | ✅ |
| user namespace | ✅ |
| devtmpfs | ✅ |
| Docker 容器 + 端口 | ✅ |
| macvlan 控制面 | ✅ |
| macvlan 数据面 | ❌（WiFi 单 MAC 限制，非模块问题） |
| IPv6 NAT 规则 | ✅ |
| pids 显示层（检查项变绿） | ✅ |
| pids 功能层（fork 拦截） | ✅（`refused` > 0，模拟实现，见 §9） |
