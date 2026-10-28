> English version of [VERIFY.md](../../VERIFY.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# Post-Deployment Complete Verification Checklist

Environment: OnePlus Ace 6 Ultimate Edition `PMB110`, system `17.0.0.101(CNO1C10P02)`,
kernel `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`.
Recorded: 2026-10-27.

All commands are run as `su -c '...'` or in a root shell.

---

## 1. Module Load Status

```sh
cat /proc/modules
```

Expected: 6 lines visible:

```
droid_lkm        ...
droid_lkm_vendor ...
droid_lkm_misc   ...
droid_lkm_pids   ...
macvlan          ...
ip6table_nat     ...
```

Confirm in dmesg that there is no `Unknown symbol` / `CFI failure`:

```sh
dmesg | grep -iE 'droid_lkm|macvlan|ip6nat|Unknown symbol|CFI failure' | tail -20
```

---

## 2. namespace / IPC (droid_lkm)

```sh
ls -l /proc/self/ns/{pid,ipc,mnt,uts}
unshare --pid --fork --mount-proc /bin/sh -c 'echo PIDNS_OK; ls /proc | head -3'
```

Expected: inside the container `echo $$` is 1 / a small pid; `/proc/self/ns/ipc` is readable.

SysV IPC:

```sh
ipcs -m
ipcs -q
```

Passes as long as there is no `Invalid argument`.

---

## 3. user namespace (droid_lkm_misc)

```sh
readlink /proc/self/ns/user
cat /proc/self/uid_map
cat /proc/self/gid_map
```

Expected: `uid_map` is **non-empty** (`0 0 4294967295`), `gid_map` has content.
This is the key to whether "sandbox allowed" can be turned on.

---

## 4. devtmpfs (droid_lkm_misc)

```sh
cat /proc/filesystems | grep devtmpfs
```

Expected output:

```
nodev	devtmpfs
```

It only registers, no thread is started; mounting and node population are done by
Droidspaces itself.

---

## 5. Full Docker Run

```sh
docker run --rm hello-world
```

Expected: `hello-world` outputs normally.

Background container + port:

```sh
docker run -d --name deepseek_harness -p 8080:8080 <image>
docker ps
```

Expected: `STATUS` shows `Up (healthy)`.

Namespaces inside the container:

```sh
docker exec deepseek_harness ls -l /proc/self/ns/{pid,ipc,user}
```

---

## 6. macvlan (control plane)

```sh
ip link add mvt link wlan0 type macvlan mode bridge
ip link show mvt
ip addr add 10.131.204.250/24 dev mvt
ip link set mvt up
ip link del mvt
```

Expected: the interface can be created and deleted without errors.

**Data plane test (does not pass on this device, which is expected)**:

```sh
ping -c 2 -I mvt 10.131.204.1
ip -s link show mvt
```

Actual output:

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

**Verdict**: TX errors=3, RX=0, ARP always FAILED — this is caused by the
**onboard WiFi driver accepting only a single MAC**, not a module bug. It works with a
USB wired NIC. host network mode does not need macvlan.

---

## 7. IPv6 NAT (ip6table_nat)

```sh
ip6tables -t nat -L -n
ip6tables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE
ip6tables -t nat -L -n
ip6tables -t nat -D POSTROUTING -o wlan0 -j MASQUERADE
```

Expected: the table can be listed, rules can be added and deleted, and there is no
`iptables: Table does not exist`.

---

## 8. Droidspaces App Side

Edit the container, confirm that both "sandbox allowed" and "hardware access" can be
turned on, and that the App reports `Sandboxing: enabled` and `HW access: full`.

If the App shows "hardware access unavailable" while `/proc/filesystems` already has
`devtmpfs`, it is App cache — clear it:

```sh
su -c 'am force-stop com.droidspaces.app'
su -c 'sed -i -e "/name=\"host_capabilities\"/d" \
        -e "/name=\"host_capabilities_boot_id\"/d" \
        /data/data/com.droidspaces.app/shared_prefs/droidspaces_prefs.xml'
```

---

## 9. pids controller (`droid_lkm_pids`)

> The kernel is not built with `CONFIG_CGROUP_PIDS`; this is an **emulated implementation**.
> For the principle, accounting-point selection, and known deviations, see
> [`PIDS-ENFORCEMENT.md`](PIDS-ENFORCEMENT.md).

### 9.1 Probe Self-Check After Loading

```sh
su -c 'dmesg | grep droid_lkm_pids | tail -12'
```

Expected (addresses vary per device):

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

The key lines are `layout verified` and `pids limit enforcement active`. Only when it says
`layout unverified` or `enforcement incomplete` does the function layer not take effect
(leaving only the display layer).

### 9.2 App Side

```sh
docker run --rm --pids-limit 256 hello-world
```

Expected: it **no longer** reports `pids.max is not available`; the `Process limit support`
check item passes.

```sh
su -c 'ls /sys/fs/cgroup/<容器目录>/ | grep pids'
```

Expected: `pids.max` / `pids.current` / `pids.stats` appear.

### 9.3 Whether the Function Layer Is Really Blocking: Check `refused`

`refused` is "the number of forks blocked and returned `-EAGAIN`".
**Greater than 0 means the function layer is in effect.**

```sh
su -c 'cat /sys/fs/cgroup/<容器目录>/pids.stats'
```

```
subtree=3644 strip=48 rewrite=36 fork=10530 charged=10530 refused=629 rmdir=378 layout=1
```

`layout` must be `1`; `fork` should keep growing (it is the system-wide fork count,
not the count inside the container).

### 9.4 Manual cgroup Control Experiment (Without a Container)

```sh
su -c 'CG=/sys/fs/cgroup/t2; rmdir $CG 2>/dev/null; mkdir -p $CG; echo 5 > $CG/pids.max; echo $$ > $CG/cgroup.procs; read S < $CG/pids.stats; echo "start   $S"; i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); read C < $CG/pids.current; read S < $CG/pids.stats; echo "第${i}次 current=$C  $S"; done; rmdir $CG 2>/dev/null'
```

Measured (`PMB110` / `6.12.69`):

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

Verdict:

- `current` cannot grow past 4 → the quota is exhausted;
- `refused` **+5** between iterations 4→5 → some forks were indeed rejected with `-EAGAIN`;
- the ~6 second pause between iterations 4→5 is **not a hang**: `/system/bin/sh` waits and
  retries when `fork()` returns `EAGAIN`, continuing only after the first batch of
  `sleep 6` expires and releases quota, so `current` drops back to 1 and climbs again.

### 9.5 Stability

```sh
su -c 'dmesg | grep -iE "droid_lkm_pids|Unable|panic|BUG:" | tail -20'
```

Expected: no `Unable to handle kernel paging request` / `panic` / `BUG:` / `CFI failure`.

---

## 10. Result Summary

| Item | Verdict |
| --- | --- |
| 5 modules loaded | ✅ |
| PID/IPC namespace | ✅ |
| SysV IPC / POSIX mqueue | ✅ |
| user namespace | ✅ |
| devtmpfs | ✅ |
| Docker container + port | ✅ |
| macvlan control plane | ✅ |
| macvlan data plane | ❌ (WiFi single-MAC limitation, not a module issue) |
| IPv6 NAT rules | ✅ |
| pids display layer (check item turns green) | ✅ |
| pids function layer (fork interception) | ✅ (`refused` > 0, emulated implementation, see §9) |
