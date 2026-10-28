# 07 · 自己构建：把这套东西适配到你的手机

前 6 篇都假设你的手机是**一加 Ace 6 至尊版（PMB110 / 6.12.69）**。
这一篇反过来：**你手上是别的机器，怎么把同一套源码编成能 `insmod` 的 ko。**

> 先说清楚代价：这不是"改个机型名就能跑"。换机型 = 换内核 = 换编译器 = 重新验证会不会崩。
> 本文所有命令都在 Linux 主机（或 WSL2）上跑，不在手机上编译。

---

## 0. 先判断值不值得动手

四个前提，缺一个就别往下走：

```sh
su -c 'uname -r'
su -c 'getprop ro.product.device; getprop ro.product.model'
su -c 'zcat /proc/config.gz | grep -E "^CONFIG_(MODULES|MODULE_SIG_FORCE|MODULE_SIG|CFI_CLANG|KCFI|PID_NS|IPC_NS|USER_NS|DEVTMPFS|SYSVIPC)="'
su -c 'ls /data/adb/ksu 2>/dev/null || ls /data/adb/ap 2>/dev/null'
```

| 前提 | 合格 | 不合格的后果 |
|---|---|---|
| 已 root（KernelSU / APatch） | `/data/adb/ksu` 或 `/data/adb/ap` 存在 | 装不了模块，结束 |
| 是 GKI 内核 | `uname -r` 里带 `android12` / `android13` … `android16` 字样 | 没有 KMI 概念，DDK 覆盖不到 |
| 允许加载外部模块 | `CONFIG_MODULES=y` 且 `CONFIG_MODULE_SIG_FORCE` 不是 y | `insmod: invalid module format`，无解 |
| 内核源码可得 | 厂商开源仓库或 GKI 对应分支 | 拿不到匹配的 `Module.symvers`，编出来也加载不了 |

顺带看一眼这三个，它们决定"要补什么"：

```sh
# 缺 PID_NS / IPC_NS → 正是 droid_lkm.ko 要补的
# 缺 DEVTMPFS → droid_lkm_misc.ko 补
# 缺 USER_NS → 补不了，别试（见 README 开头那一节）
```

> ⚠️ `CONFIG_CFI_CLANG=y` / `CONFIG_KCFI=y` 的机器，**编译器版本必须和目标内核一致**。
> 用错 clang 编出来的 ko，`insmod` 时不是报错，是 `CFI failure` + **整机断电**。
> 这不是危言耸听，本项目的 PMB110 上实测过两次。

---

## 1. 认出你的 KMI，选对 DDK

KMI 就是内核模块接口版本，格式 `android<代数>-<主>.<次>`。看 `uname -r` 对号入座：

| `uname -r` 长这样 | KMI / 构建 target |
|---|---|
| `6.12.x-android16-…` | `android16-6.12` |
| `6.6.x-android15-…` | `android15-6.6` |
| `6.1.x-android14-…` | `android14-6.1` |
| `5.15.x-android14-…` | `android14-5.15` |
| `5.15.x-android13-…` | `android13-5.15` |
| `5.10.x-android13-…` | `android13-5.10` |
| `5.10.x-android12-…` | `android12-5.10` |
| `6.18.x-android17-…` | `android17-6.18` |

`scripts/build-ddkk.sh` 的第一个参数就是 target，镜像 tag 同名：

```
docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12
```

> 如果对应 tag 拉不下来，就不用镜像，走第 4 节自建 KDIR。

---

## 2. 编一遍

```sh
git clone https://github.com/ai-luo2006/droidspaces-6.12-pmb110.git
cd droidspaces-6.12-pmb110/src

sh scripts/fetch-deps.sh          # 拉 4 个第三方工程（有离线快照可跳过）
sh scripts/build-ddkk.sh android15-6.6   # ← 换成你的 target
```

产物：

```
droid_lkm/out/<target>/droid_lkm.ko
droid_lkm/out/<target>/droid_lkm_vendor.ko   ← 仅 6.6 / 6.12
droid_lkm/out/<target>/droid_lkm_misc.ko     ← 仅 6.6 / 6.12
droid_lkm/out/<target>/droid_lkm_pids.ko     ← 仅 6.6 / 6.12
```

**这里有个硬限制，必须知道。** `droid_lkm/Makefile` 里：

```make
VENDOR_VER := $(shell sed -n 's/.*UTS_RELEASE "\([0-9]*\.[0-9]*\).*/\1/p' $(KDIR)/include/generated/utsrelease.h)
DLKM_OPT := $(strip $(if $(filter 6.6 6.12,$(VENDOR_VER)), \
	$(shell grep -q ' nop_mnt_idmap$' $(KDIR)/System.map && echo m),))
obj-$(DLKM_OPT) += droid_lkm_vendor.o droid_lkm_misc.o droid_lkm_pids.o
```

翻译过来：

| 你的内核 | `droid_lkm.ko`（pid/ipc ns） | `vendor` / `misc`（devtmpfs）/ `pids` |
|---|---|---|
| 6.12.x | ✅ 已验证 | ✅ 会构建 |
| 6.6.x | ⚠️ 可构建，**未真机验证** | ✅ 会构建 |
| 6.1 / 5.15 / 5.10 | ⚠️ 可构建，**未真机验证** | ❌ **不会构建**，Makefile 直接不产出 |
| 5.4 及更老、非 GKI | ❌ | ❌ |

也就是说：**在 6.1 及更老的机器上，你只能拿到容器命名空间那一半能力，devtmpfs 和 pids 限制补不上。**
要补，得自己把 `src/misc/` 和 `src/pidsmod/` 里的结构体布局按那个内核改一遍 —— 那是另一个量级的活。

> 6.6 那一行的"会构建"还有个附加条件：`KDIR/System.map` 里要有 `nop_mnt_idmap` 这个符号
> （GKI 6.6 有）。没有的话 `DLKM_OPT` 为空，一样只产出主模块。
> 编完先 `ls out/<target>/`，看实际出来几个 ko，别只看表格。

---

## 3. 编不过的三种报错

| 报错 | 原因 | 修法 |
|---|---|---|
| `No rule to make target '.../vendorcompat/deps.mk'` | `.sdk/builtin/` 没跟着拷 | 从完整源码包解压，别只拷 `src/` |
| `Unknown symbol xxx (err -2)` | 用了内核**未导出**的符号 | 参考 `net_macvlan/macvlan_shim.h`：用 `-include` + 宏改名，运行时靠 KallRecon 从 kallsyms 查地址 |
| `CFI failure at ...` 然后关机 | clang 版本和内核不匹配 | 用 DDK 镜像里的 clang，不要用系统自带的 |
| `No targets. Stop.`（网络模块） | 那是 Kbuild 片段，不能直接 `make` | `make -C $KDIR M=/src modules` |
| `undefined reference to __stack_chk_fail` 之类 | 缺 `-fno-stack-protector` | 跟随 DDK 的 `ccflags-y`，别自己加编译参数 |

---

## 4. 拿不到 DDK 镜像：自建 KDIR

KDIR 不是"内核源码目录"，而是**编译过的内核输出目录**，里面必须有两个东西：
`include/generated/`（含 `utsrelease.h`、`autoconf.h`）和 `Module.symvers`。

```sh
# 1) 取目标机型的开源内核（厂商仓库，或 kernel/common 对应分支）
git clone --depth 1 -b <你的分支> <厂商内核仓库> kernel_src

# 2) 用厂商 defconfig 编一次（GKI 用 gki_defconfig，高通/联发科用 vendor defconfig）
cd kernel_src
make O=../kout ARCH=arm64 LLVM=1 <你的_defconfig>
make -C . O=../kout ARCH=arm64 LLVM=1 -j$(nproc) Image modules

# 3) ../kout 就是 KDIR
ls ../kout/Module.symvers ../kout/include/generated/utsrelease.h
```

然后手动编，把 `KDIR` 指过去：

```sh
cd src/droid_lkm
make -C $KDIR M=$PWD VER=<target> modules
```

> 关键：**编内核用的 clang 必须和编模块用的是同一个**。`make LLVM=1` 会调用 `clang`，
> 版本不一致照样 KCFI 崩。查版本：`clang --version`，和内核构建日志里的对比。

---

## 5. 让模块认你的手机（三处守卫）

模块自带三道锁，全在 `src/dsm/post-fs-data.sh` 里。默认只认 PMB110 + 6.12.69。

**① 版本守卫**

```sh
case "$REL" in
  6.12.69-*) : ;;
  *) say "内核 $REL 与目标 6.12.69-* 不符, 不加载"; exit 0 ;;
esac
```

**② 机型守卫**

```sh
case "$FINGER" in *PMB110*) ok=1 ;; esac
```

**③ vermagic 自检**

```sh
grep -aq "vermagic=6.12" "$KO/droid_lkm.ko" || bad="$bad vermagic-not-6.12"
```

有两种改法，选一种：

### 改法 A：不动脚本，用开关文件（推荐先这么试）

```sh
su -c 'touch /data/adb/modules/Droidspaces/allow_any_kernel'
su -c 'echo "<你的 ro.product.device>" > /data/adb/modules/Droidspaces/allow_list'
```

但 ③ 的 `vermagic=6.12` 是写死的字符串，非 6.12 的机器过不了，得再加：

```sh
su -c 'touch /data/adb/modules/Droidspaces/no_precheck'   # 跳过自检（含 vermagic）
```

### 改法 B：直接改脚本，重新打包

```sh
# 换内核版本
sed -i 's/6\.12\.69-\*/6.6.*-*/' src/dsm/post-fs-data.sh
# 换机型代号
sed -i 's/\*PMB110\*/*你的代号*/' src/dsm/post-fs-data.sh
# 换 vermagic 前缀
sed -i 's/vermagic=6\.12/vermagic=6.6/' src/dsm/post-fs-data.sh
```

顺手把 `src/dsm/module.prop` 的 `id` / `name` 改掉，否则会和原版模块互相覆盖：

```ini
id=Droidspaces-<你的代号>
name=Droidspaces 6.6 (<你的机型>)
```

---

## 6. 第一次加载：一块一块上，别一次全上

**这是全篇最重要的一节。** 一次 `insmod` 六个模块，崩了你不知道是哪个。

### 6.1 先只上主模块

```sh
su -c 'insmod /data/adb/modules/Droidspaces/ko/droid_lkm.ko \
  gate=0 mqueue=1 nspid=1 nspid_append=1 skip_sysvipc=0 no_fake_ns=0 skip_do_exit=0'
su -c 'dmesg | grep -i droid_lkm | tail -30'
```

看到初始化日志、手机没重启，才算第一步过了。**这一步崩，后面全部不用试。**

### 6.2 再加 vendor / misc / pids

```sh
su -c 'insmod .../droid_lkm_vendor.ko ghost=1 ghost_match=oplus_ pid_max_cap=0'
su -c 'insmod .../droid_lkm_misc.ko'
su -c 'insmod .../droid_lkm_pids.ko'
```

每加一个都看一眼 `dmesg`。`droid_lkm_misc.ko` 是历史上最容易崩的一个（userns 那套伪造逻辑），
非 6.12 的内核上如果它一加载就重启，**直接放弃它**，只留主模块也能用容器。

### 6.3 网络模块最后加

```sh
su -c 'insmod .../macvlan.ko'      # 失败会打印取不到符号的名字
su -c 'insmod .../ip6table_nat.ko'
```

这两个自带符号自解析，取不到会明确告诉你缺哪个符号，不会静默崩。

### 6.4 崩了怎么救

| 症状 | 救法 |
|---|---|
| 加载后立刻重启，开机又重启 | KernelSU 安全模式（开机按住音量下），或 adb `rm -rf /data/adb/modules/Droidspaces` |
| 能开机，但模块卡住开机流程 | 关机，进 recovery/TWRP 删目录；或有 `blocked` 文件时脚本会自己跳过 |
| `insmod` 返回 `-22` EINVAL | 参数名不对（版本差异），去掉参数裸加载 |

> 调试期**一定要留着 `no_boot` 文件**（包里默认就有）。它让模块开机不自动加载，
> 崩了重启还能进系统。验证通过后再删。

---

## 7. 打包

```sh
sh scripts/package-ksu-zip.sh
# → dist/Droidspaces-<version>.zip
```

zip 根目录必须直接是 `module.prop` / `post-fs-data.sh` / `ko/`，**不能多套一层文件夹**，否则 KernelSU 装不上。

刷入后落在 `/data/adb/modules/Droidspaces/`，检查：

```sh
su -c 'ls /data/adb/modules/Droidspaces/ko/'
su -c 'cat /data/adb/modules/Droidspaces/load.log'
```

---

## 8. 验收清单

六项全过，才算"适配成功"：

| # | 检查 | 命令 | 期望 |
|---|---|---|---|
| 1 | 六个模块在线 | `grep -c "^droid_lkm\|^macvlan\|^ip6table" /proc/modules` | 5~6 |
| 2 | 没有未知符号 | `dmesg \| grep -i "unknown symbol"` | 无输出 |
| 3 | 容器能起 | App 里点启动 | `(RUNNING)` |
| 4 | 容器能进 | `droidspaces --name=root enter` | 拿到 shell |
| 5 | pid ns 独立 | 容器内 `readlink /proc/self/ns/pid` vs 宿主 | 两个 inode 不同 |
| 6 | 连续重启 3 次不崩 | 反复 `stop` / `start` | 每次都成功 |

第 6 项最容易忽略：**能起来不等于稳定**。本项目 v1.0.18 就是"能起、跑一会儿重启"，
原因是 fork 路径上换了 cred。至少跑 30 分钟再做结论。

---

## 9. 什么情况下别折腾了

诚实列一下"改也改不出来"的情况：

- **内核没开 `CONFIG_MODULES`**，或 `CONFIG_MODULE_SIG_FORCE=y` 且你没有厂商签名 —— 物理上装不进去。
- **内核把 `ksys_unshare` / `kernel_clone` 改名或内联掉了** —— kallsyms 里找不到，钩子挂不上，主模块加载即失败。
- **开了内核完整性校验**（部分厂商的 `CONFIG_QCOM_KERNEL_INTEGRITY` 之类）—— 加载未签名模块会被拦。
- **内核 < 5.4 或非 GKI** —— `struct task_struct` 布局和本项目假设差太多，`ds_layout_anchor.c` 的锚点扫描也救不回来。
- **想要 userns 隔离** —— 唯一的路是重编内核开 `CONFIG_USER_NS`，那就不是"模块"方案了，超出本项目边界（原因见 README 开头）。

---

## 10. 一句话总结

> 移植的难点不在写代码，在**编译器版本**和**符号可见性**这两件事上。
> 只要 clang 对得上、模块用到的符号能从 kallsyms 查到，剩下的就是逐块加载 + 观察 `dmesg`。
