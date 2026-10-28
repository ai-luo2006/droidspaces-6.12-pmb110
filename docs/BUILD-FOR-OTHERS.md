# 为别人的手机构建（适配流程）

这一节回答："我把这个仓库给朋友了，他怎么给自己手机编译出能用的 ko？"

---

## 0. 一句话结论

**构建必须用"他自己手机的"内核作为 KDIR，并且必须用与该内核一致的 clang。**
本仓库里现成的 `prebuilt/*.ko` 只在 PMB110 / 6.12.69 上验证过，别人**不能直接用**。

---

## 1. 先摸清对方手机的 3 个参数

在对方手机上（已 root）执行：

```sh
uname -r                                   # 内核版本串
getprop ro.build.version.release           # Android 大版本
getprop ro.product.device                  # 机型代号
getprop ro.product.model                   # 机型名
cat /proc/version                          # 编译器的详细版本
```

本机（一加 Ace 6 至尊版）的实测值：

```
uname -r  = 6.12.69-android16-6-g242e1a07f878-ab15865446-4k
release   = 17
device    = PMB110
model     = 一加 Ace 6 至尊版
```

---

## 2. 判断走哪条路

### 路线 A：机型与已验证组合一致 → 直接用预编译 ko

只有完全命中下面全部条件才能用 `prebuilt/` 里的 ko：

- 机型代号含 `PMB110`
- 内核 `uname -r` 以 `6.12.69-` 开头

满足则：直接把 `dsm/` 打成 zip 装上即可，无需任何编译。

### 路线 B：同一内核线，不同机型 → 重新编译主模块 + 放开机型锁

内核还是 `6.12.69-*`（或其它 GKI 6.12），只是机型不同：

```bash
# 1. 用同一个 DDK 镜像编译（内核版本相同 → 编译参数相同）
sh scripts/build-ddkk.sh android16-6.12
sh scripts/build-net-ddk.sh android16-6.12

# 2. 打包
sh scripts/package-ksu-zip.sh

# 3. 装机后放开机型锁（在模块目录里）
su -c 'echo "<对方的机型代号>" >> /data/adb/modules/Droidspaces/allow_list'
```

> `allow_list` 支持**子串**匹配，所以填 `PMB110` 之类的前缀也行。
> 匹配的是 `ro.product.device / model / board / odm.device / vendor.device /
> boot.product.hardware.sku / device-tree model` 的拼接指纹。

### 路线 C：不同内核版本（6.6 / 6.1 / 5.15 …）→ 换 DDK target 重新编译

DDK 镜像按 Android+内核版本分 tag，一个 tag 对应一个 KDIR：

| DDK tag | KDIR 内核版本串 | 对应 Android |
| --- | --- | --- |
| `android17-6.18` | 6.18.x | Android 17 |
| `android16-6.12` | `6.12.x`（DDK 镜像自带的可能是 6.12.76；本仓库的 PMB110 是 6.12.69，同属这个 tag） | Android 16 |
| `android15-6.6` | `6.6.127-4k-g46a034eca005` | Android 15 |
| `android14-6.1` | `6.1.166` | Android 14 |
| `android14-5.15` | 5.15.x | Android 14 |
| `android13-5.15` | `5.15.202-android13-5.15.202_r00` | Android 13 |
| `android13-5.10` | 5.10.x | Android 13 |
| `android12-5.10` | 5.10.x | Android 12 |

用法：

```bash
sh scripts/build-ddkk.sh    android15-6.6
sh scripts/build-net-ddk.sh android15-6.6
sh scripts/package-ksu-zip.sh
```

换 target 后还要注意：

1. **模块源码有版本分支**。`droid_lkm/Makefile` 里有 `VENDOR_VER` 判断
   （只支持 6.6 / 6.12），以及按 `System.map` 里符号是否存在来挑
   `munmap` 实现单元（`munmap_vmi.o` / `munmap_mas.o` / `munmap_legacy.o`）。
   这套逻辑是自动的，不用手改。
2. **网络模块的 shim 符号可能变**。6.12 与 6.6 的 macvlan / ip6tables
   未导出符号集合不完全一样。若编译报 `Unknown symbol`，说明该内核版本
   需要另一套 shim 名单，得按报错补。
3. **vermagic 必须对得上**。装不上时先看：
   ```sh
   strings /data/adb/modules/Droidspaces/ko/droid_lkm.ko | grep vermagic
   ```
   与 `uname -r` 的 `major.minor.patch` 一致才行。

### 路线 D：厂商深度定制内核（最麻烦）

有些机型（如本机一加/OPPO）的内核不是纯 GKI，而是厂商深度改过的：

- 内核版本串带 `-android16-6-g242e1a07f878-ab15865446-4k` 这种后缀
- 厂商自己合了补丁、动了结构体布局
- 可能用了非公开的 clang 版本

这种情况**必须用该机型对应的内核源码树自己编 KDIR**：

```bash
# 1. 拿到该机型的内核源码（厂商开源仓库 / 内核源码 zip）
git clone <该机型内核仓库> ksrc && cd ksrc

# 2. 用与该内核一致的 clang（看 /proc/version 里的 clang 版本）
#    Android 16 GKI 用的是 clang 19；版本不一致会导致 KCFI 失败

# 3. 造 KDIR
make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gki_defconfig
scripts/config --file .config --enable CONFIG_USER_NS
make LLVM=1 ARCH=arm64 olddefconfig
make -j$(nproc) LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image
make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
```

得到 KDIR 后，用本仓库的编译脚本，把 KDIR 换掉：

```bash
docker run --rm \
  -e KDIR=/your/kdir \
  -v "$PWD/droid_lkm":/src -w /src \
  docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
  sh -c 'make -C $KDIR M=/src modules'
```

---

## 3. 「KCFI 必须匹配」到底是什么意思

目标内核开了 **KCFI**（clang 控制流完整性）。每个函数签名被哈希写进内核，
模块加载时内核会校验模块里每个函数的签名哈希。

- 签名不一致 → `CFI failure at do_one_initcall (target: init_module+0x0 [xxx])`
- 然后**内核直接关机**（不是报错返回）。

所以：

| 事项 | 要求 |
| --- | --- |
| clang 版本 | 必须与编译该内核的 clang 一致（6.12 GKI → clang 19） |
| KDIR | 必须是该内核编译产物（含 `Module.symvers`） |
| 函数原型 | 与内核头文件一致（本项目用 `-include` shim 强制改名，编译器会校验） |

**用系统自带的 clang 18 编出来的模块，`insmod` 会直接关机**——本项目实测踩过两次。
DDK 镜像存在的唯一理由就是提供匹配的 clang + KDIR 组合。

---

## 4. 装机后的自检清单

对方手机上按顺序验证，任一步失败就停下来查原因：

```sh
# ① 六个模块都在
grep -E '^(droid_lkm|droid_lkm_vendor|droid_lkm_misc|droid_lkm_pids|macvlan|ip6table_nat) ' /proc/modules

# ② 没有 Unknown symbol / CFI failure
dmesg | grep -iE 'Unknown symbol|CFI failure' | tail

# ③ user namespace 可用（"允许沙箱"的前提）
cat /proc/self/uid_map | head -1     # 期望 "0 0 4294967295"

# ④ devtmpfs 已注册
grep devtmpfs /proc/filesystems        # 期望 "nodev  devtmpfs"

# ⑤ Docker 能跑
docker run --rm hello-world

# ⑥ IPv6 NAT
ip6tables -t nat -L -n
```

---

## 5. 常见失败与对应动作

| 现象 | 原因 | 动作 |
| --- | --- | --- |
| `insmod: ERROR: could not insert module ... Invalid module format` | vermagic 不匹配 | 换对应内核版本的 DDK target 重编 |
| `Unknown symbol xxx` | 该内核版本未导出集合不同 | 补 shim 名单；确认 `CONFIG_TRIM_UNUSED_KSYMS` 情况 |
| `CFI failure` 后关机 | clang 版本不匹配 | 必须用 DDK 镜像的 clang 19 |
| `kernel BUG` at `if_macvlan.h` | 漏 `-DCONFIG_MACVLAN=1` | 已在 Makefile 里，检查是否被改过 |
| 脚本提示"内核不符" | 内核版本不在 `6.12.69-*` | `touch allow_any_kernel` 或改 `post-fs-data.sh` 的 case 分支 |
| 脚本提示"机型不符" | 机型不在白名单 | `echo <代号> >> allow_list` |
| 装完重启循环 | 崩溃守卫没生效 | 见 `USAGE.md` 的恢复章节 |

---

## 6. 关于「内核守卫」要不要放宽

`post-fs-data.sh` 里的守卫分两层：

```sh
# 内核层
case "$REL" in
  6.12.69-*) : ;;                  # 只放行 6.12.69
  *) exit 0 ;;                     # 其它一律不加载
esac

# 机型层
case "$FINGER" in *PMB110*) ok=1 ;; esac
```

给别人的包建议这样处理：

- **保留内核锁**。内核版本错了直接关机，风险太大。
- **机型锁改成白名单**，让对方自己加代号：
  ```sh
  # 删掉 PMB110 硬匹配，改为读 allow_list
  # 或者直接把对方代号写进 allow_list
  ```

如果确实要放宽内核锁，**必须**先把 `post-fs-data.sh` 的 `case` 改成
`6.12.*` / `6.6.*` 这样的通配，而不是让对方去建 `allow_any_kernel`——
后者等于关掉了唯一的救命闸门。
