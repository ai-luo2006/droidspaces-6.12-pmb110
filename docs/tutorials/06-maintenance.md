# ⑥ 升级 / 回退 / 卸载

目标：换版本不把手机弄坏，卸载不留垃圾。

---

## 铁律

> **卸载模块或换版本之前，先停容器。**

容器在跑的时候 `rmmod`，内核里还有进程持有模块建的命名空间对象，卸载后变野指针。
历史上有两次整机重启就是这么来的：

```
Unable to handle kernel paging request
  vfs_open → try_module_get → do_page_fault
```

v1.0.15 之后加了保护（`release` 会返回 `-EBUSY` 而不是直接崩），但**保护是兜底，不是许可**。

```sh
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'
```

---

## 1. 升级

### 方式 A：KernelSU 刷 zip（推荐）

KernelSU → 模块 → 安装 → 选新 zip → 重启。

重启后如果模块没自动加载，说明 `no_boot` 还开着（首次安装默认关闭自启），去 WebUI 点「加载」，
或者命令：

```sh
su -c 'PH_ACTION=1 sh /data/adb/modules/Droidspaces/post-fs-data.sh'
```

### 方式 B：只换内核模块（不用重启）

```sh
# 1) 停容器
su -c '/data/local/Droidspaces/bin/droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop'

# 2) 卸旧模块
su -c 'echo 1 > /sys/module/droid_lkm_misc/parameters/release; rmmod droid_lkm_misc; echo "rmmod rc=$?"'

# 3) 换文件
su -c 'cp /sdcard/Download/droid_lkm_misc.ko /data/adb/modules/Droidspaces/ko/'

# 4) 装新的
su -c 'insmod /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko real_userns=1 userns_any=1; echo "insmod rc=$?"'

# 5) 核对
su -c 'md5sum /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko'
```

`rmmod rc=0` 才算卸干净。如果是 `-EBUSY`，说明还有容器在跑或有进程持着引用，回第 1 步。

> 注意：`real_userns=1` 是**惯性参数**，模块里那套 id 翻译在当前内核上不生效
> （内核未编译 `CONFIG_USER_NS`）。带上它只是为了 App 的探测行为一致，不影响容器。

### 方式 C：只换 WebUI 页面（最轻）

```sh
su -c 'cp /sdcard/Download/webui-index.html /data/adb/modules/Droidspaces/webroot/index.html
       md5sum /data/adb/modules/Droidspaces/webroot/index.html'
```

v1.0.20 的页面 md5 是 `75f679574e0347b46eead02789b405a7`。换完退出模块页重进（不是下拉刷新）。

---

## 2. 版本对照

| 版本 | 能不能装 | 说明 |
|---|---|---|
| **v1.0.20** | ✅ 当前 | 推荐。ko 与 v1.0.19 相同，只改页面与护栏 |
| v1.0.19 | ✅ 可回退 | 修掉 clone 换凭据的崩溃路径，稳定 |
| **v1.0.18** | 🔴 **禁止** | 每次 fork 改 cred，必崩，已导致两次整机重启 |
| v1.0.17 | 🟠 半成品 | 容器 `su` 会报 `cannot set groups` |
| v1.0.16 | ⚪ 无效 | 钩子挂错位置，完全没触发 |
| v1.0.15 及更早 | ⚪ 历史 | 见 `archive/` |

**回退到 v1.0.19**：只换页面即可（ko 完全一样）。

```sh
su -c 'cp /sdcard/Download/v1.0.19-webui-index.html /data/adb/modules/Droidspaces/webroot/index.html'
```

差别只是：v1.0.19 的页面会把「真 userns」显示成「已生效」，并且「转换容器属主」按钮可以点。
**那个按钮就是坑**，回退后千万别点。

---

## 3. 卸载

### 正确顺序

```sh
# 1) 停所有容器
su -c 'for c in /data/local/Droidspaces/Containers/*/container.config; do
         /data/local/Droidspaces/bin/droidspaces --config="$c" stop 2>&1 | tail -1
       done'

# 2) 确认没有容器进程还持着 rootfs
su -c 'grep -l "Droidspaces/Containers" /proc/[0-9]*/mountinfo 2>/dev/null | head'
#    有输出 = 还有容器在跑，回去第 1 步

# 3) 恢复 config.gz 伪装（卸载脚本会做，手动确认一下）
su -c 'umount /proc/config.gz 2>/dev/null; echo "umount rc=$?"'

# 4) 卸载模块
su -c 'echo 1 > /sys/module/droid_lkm_misc/parameters/release; rmmod droid_lkm_misc; echo "misc rc=$?"'
su -c 'rmmod droid_lkm_pids droid_lkm_vendor droid_lkm ip6table_nat macvlan 2>/dev/null; grep "^droid_lkm" /proc/modules; echo "(空=全部卸载)"'
```

然后在 KernelSU 里删模块，或直接删目录：

```sh
su -c 'rm -rf /data/adb/modules/Droidspaces'
```

### 卸载后要确认的三件事

| 检查 | 命令 | 期望 |
|---|---|---|
| 模块真的没了 | `ls /sys/module/droid_lkm_misc` | 目录不存在 |
| `config.gz` 恢复原样 | `zcat /proc/config.gz \| grep CONFIG_USER_NS` | `# CONFIG_USER_NS is not set` |
| 原生 Droidspaces 还能跑 | App 里点容器 → 启动 | 能起（不走模块那套 ns） |

如果第二项还是 `=y`，说明 `fake_config.sh` 的 bind mount 没撤掉，重启一次即可恢复。

---

## 4. 关于 rootfs 属主

**不要在卸载前 relabel。** 当前状态磁盘属主就是 `0`，跟原生 Droidspaces 完全兼容，卸载后容器照样能起。

只有一种情况需要 `restore`：你曾经 relabel 过、磁盘上还有 200000 的文件。

```sh
su -c 'sh /data/adb/modules/Droidspaces/userns.sh check all'
```

输出「还有 0 项」= 干净，什么都不用做。

---

## 5. 升级后自检清单

```sh
su -c 'echo "模块: $(grep -c "^droid_lkm" /proc/modules)/4"
       echo "版本: $(grep -m1 ^version= /data/adb/modules/Droidspaces/module.prop)"
       echo "页面: $(md5sum /data/adb/modules/Droidspaces/webroot/index.html | cut -d" " -f1)"
       echo "存储直通: $(grep ^enable_android_storage= /data/local/Droidspaces/Containers/root/container.config)"
       echo "容器自启: $(grep ^run_at_boot= /data/local/Droidspaces/Containers/root/container.config)"'
```

v1.0.20 的期望值：

```
模块: 4/4
版本: version=v1.0.20-6.12
页面: 75f679574e0347b46eead02789b405a7
存储直通: enable_android_storage=0
容器自启: run_at_boot=0
```

然后起容器、进容器跑 `id` 和 `ls -ln /`，属主应为 `0 0`，`su -c id` 应为 `uid=0(root)`。

---

## 6. 千万别做的四件事

| 动作 | 后果 |
|---|---|
| 容器在跑时 `rmmod` | 大概率 `-EBUSY`，小概率内核崩、整机重启 |
| 点「转换容器属主」/ `userns.sh relabel` | 52 万文件改属主，容器立刻 `su: cannot set groups`，要 130 秒 restore |
| 装 v1.0.16 ~ v1.0.18 | 崩溃路径还在，会重启 |
| 手动 `insmod ... userns_adopt=1` | 同上，第二条崩溃路径 |

---

上一篇 → [⑤ 传文件与备份](05-files.md) ｜ 回 [教程目录](README.md) ｜ 卡住了 → [TROUBLESHOOTING](../TROUBLESHOOTING.md)
