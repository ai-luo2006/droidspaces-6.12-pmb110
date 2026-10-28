# 从源码到 .ko 的完整构建流程

本文档记录从零到产出 6 个 ko 的**全部**步骤，可复现。

---

## 0. 为什么必须用 Android DDK

目标内核 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` 开启了 **KCFI**
（clang 的控制流完整性）。KCFI 会校验函数签名（type hash）。
模块里任何函数原型与内核编译时不一致，`insmod` 时立刻报：

```
CFI failure at do_one_initcall (target: init_module+0x0 [macvlan])
```

然后**直接关机**。这不是软失败，是硬 panic。

所以：

- 必须用**与目标内核匹配的 clang**（本例 clang 19）。
- 必须用匹配该内核的 **KDIR**（本例 `android16-6.12`）。
- 系统自带的 clang 18 编出来的 ko，**insmod 会关机**（已实测踩过两次）。

DDK 镜像同时提供了两者：

```
docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12
  KDIR = /opt/ddk/kdir/android16-6.12
  clang 19
```

---

## 1. 环境要求

| 组件 | 版本 / 来源 |
| --- | --- |
| Docker | 任意可用版本（仅用于跑 DDK 镜像） |
| 镜像 | `docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12` |
| 编译器 | 镜像内置 clang 19（**不要用宿主机 clang**） |
| KDIR | `/opt/ddk/kdir/android16-6.12`（镜像内置） |
| 交叉工具链 | 镜像内置 `aarch64-linux-gnu-` |
| 主机内核源码 | `android16-6.12` GKI 分支 commit `242e1a07f878`（可选，仅用于对照/自建 DDK） |

拉取镜像：

```bash
docker pull docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12
```

---

## 2. 编译三个主模块（droid_lkm / vendor / misc）

### 2.1 拉取第三方依赖

主模块依赖 4 个第三方工程（另有 1 项 `vendorcompat` 走 SDK 内置，没有 rev），
版本在 `droid_lkm/deps.lst` 里钉死：

```
# <name> <rev>
KallRecon b258fb6
Type_info 7182dae
KernCall cdec722
HooKern 7a02afa
vendorcompat -
```

拉取（依赖 KMSDK）：

```bash
sh scripts/fetch-deps.sh
```

或者手动（等价）：

```bash
git clone https://github.com/Dere3046/KallRecon droid_lkm/deps/KallRecon
git -C droid_lkm/deps/KallRecon checkout b258fb6
git clone https://github.com/Dere3046/Type_info  droid_lkm/deps/Type_info
git -C droid_lkm/deps/Type_info  checkout 7182dae
git clone https://github.com/Dere3046/KernCall  droid_lkm/deps/KernCall
git -C droid_lkm/deps/KernCall  checkout cdec722
git clone https://github.com/Dere3046/HooKern   droid_lkm/deps/HooKern
git -C droid_lkm/deps/HooKern   checkout 7a02afa
```

依赖目录最终长这样：

```
droid_lkm/deps/KallRecon/   (含 lib/ 全部 .c/.h/.S)
droid_lkm/deps/Type_info/
droid_lkm/deps/KernCall/
droid_lkm/deps/HooKern/
```

此外 `deps.lst` 里还有一项 `vendorcompat -`（无 rev，走 SDK 内置）。
它位于：

```
droid_lkm/.sdk/builtin/vendorcompat/
```

`droid_lkm/.sdk/builtin/` 目录**已随包附带**（约 520 KB），
`mk/deps.mk` 会自动优先用 `deps/<Name>/`，找不到时回退到
`.sdk/builtin/<Name>/`。所以**离线也能完整编译**，不需要联网。

> 少了 `.sdk/builtin/vendorcompat/deps.mk` 会报：
> `No rule to make target '/src/.sdk/builtin/vendorcompat/deps.mk'. Stop.`

### 2.2 用 DDK 编译

```bash
sh scripts/build-ddkk.sh android16-6.12
```

脚本内部等价于：

```bash
docker run --rm \
  -e KDIR=/opt/ddk/kdir/android16-6.12 \
  -e DROID_LKM_LAYOUT_FALLBACK \
  -e DROID_LKM_LAYOUT_FORCE_BUILD \
  -e DROID_LKM_LAYOUT_MANUAL \
  -e DROID_LKM_LAYOUT_TEST_BUILD_SHIFT \
  -v "$PWD/droid_lkm":/src -w /src \
  docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
  sh -c 'make VER="$1" clean 2>/dev/null; make VER="$1"' sh android16-6.12
```

产物在 `droid_lkm/out/android16-6.12/`：

```
droid_lkm.ko
droid_lkm_vendor.ko
droid_lkm_misc.ko
```

> `VER` 同时决定 KMI 选择与输出目录，**必须与目标内核一致**。

### 2.3 若要自建 DDK（不使用现成镜像）

需要先取 GKI 源码并编译出 `Module.symvers`：

```bash
git init kernel_src && cd kernel_src
git remote add origin https://github.com/aosp-mirror/kernel_common
git fetch --depth 1 origin 242e1a07f878
git checkout -q FETCH_HEAD

make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gki_defconfig
scripts/config --file .config --enable CONFIG_USER_NS
make LLVM=1 ARCH=arm64 olddefconfig
make -j$(nproc) LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image
```

得到 `Module.symvers` 后，把 DDK 镜像的 `/opt/ddk/kdir/android16-6.12`
替换成这份内核树的 `source + out`。

---

## 3. 编译两个网络模块（macvlan / ip6table_nat）

这两个**必须**用 DDK（同 KCFI 原因）。

```bash
sh scripts/build-net-ddk.sh android16-6.12
```

等价于：

```bash
for m in net_macvlan net_ip6nat; do
  docker run --rm \
    -e KDIR=/opt/ddk/kdir/android16-6.12 \
    -v "$PWD/$m":/src -w /src \
    docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
    sh -c 'make -C $KDIR M=/src clean >/dev/null 2>&1; make -C $KDIR M=/src modules'
done
```

> ⚠ 必须用 `make -C $KDIR M=/src modules` 这种 **Kbuild** 形式。
> 直接在源码目录跑 `make` 会报 `No targets. Stop.`（因为 Makefile 是
> Kbuild 片段，没有独立目标）。

实测输出（macvlan）：

```
  CC [M]  /src/macvlan_core.o
  CC [M]  /src/macvlan_shim.o
  CC [M]  /src/kr/core.o
  ...
  AS [M]  /src/kr/anchor.o
  LD [M]  /src/macvlan.o
  MODPOST /src/Module.symvers
  LD [M]  /src/macvlan.ko
  BTF [M] /src/macvlan.ko
```

产物：

```
net_macvlan/macvlan.ko
net_ip6nat/ip6table_nat.ko
```

脚本会自动把它们拷到 `dsm/ko/`。

### 3.1 macvlan 的关键编译开关

`net_macvlan/Makefile`：

```make
obj-m := macvlan.o
macvlan-y := macvlan_core.o macvlan_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

# -include : 7 个未导出符号重定向
# -DCONFIG_MACVLAN=1 : 让 if_macvlan.h 走真实实现，而不是 BUG() 存根
CFLAGS_macvlan_core.o := -include $(src)/macvlan_shim.h -DCONFIG_MACVLAN=1
CFLAGS_macvlan_shim.o := -DCONFIG_MACVLAN=1

ccflags-y += -Wno-declaration-after-statement -Wno-unused-variable \
	-Wno-unused-function -Wno-strict-prototypes
ccflags-y += -I$(src)/kr
```

> ⚠ **`-DCONFIG_MACVLAN=1` 是必须的**。少了它，`linux/if_macvlan.h:82` 会命中
> 未编译该配置时的 `BUG()` 存根，`insmod` 直接 `kernel BUG`。
> 这是本项目第一版翻车点。

### 3.2 ip6table_nat 的关键点

`net_ip6nat/Makefile`：

```make
obj-m := ip6table_nat.o
ip6table_nat-y := ip6table_nat_core.o ip6t_nat_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

CFLAGS_ip6table_nat_core.o := -include $(src)/ip6t_nat_shim.h
ccflags-y += -I$(src)/kr
```

`ip6t_register_table()` 的第四个参数在 6.12 上必须是
`const struct nf_hook_ops *`，传错类型会编译通过但运行期行为不对。

---

## 4. 打包成 KernelSU 可安装 zip

```bash
sh scripts/package-ksu-zip.sh
```

产物：`dist/Droidspaces-<version>.zip`，zip 根目录直接是模块内容
（`module.prop` / `post-fs-data.sh` / `ko/` …），KernelSU 可直接安装。

产物结构即最终安装目录：

```
Droidspaces/
├── module.prop
├── post-fs-data.sh
├── service.sh
├── uninstall.sh
├── desc.sh
├── ko/{droid_lkm,droid_lkm_vendor,droid_lkm_misc,droid_lkm_pids,macvlan,ip6table_nat}.ko
├── mirror.sh
└── webroot/index.html
```

刷入后落到 `/data/adb/modules/Droidspaces/`。

---

## 5. 编译 / 重编 `droid_lkm_pids`

pids 模块**随 §2 一起产出**（`scripts/build-ddkk.sh` 的 `make` 目标里就有它），
通常不需要单独处理。只在改了 `src/pidsmod/` 之后需要重编，此时不动其他五个模块：

```bash
sh scripts/build-ddkk.sh android16-6.12
# 产物：droid_lkm/out/android16-6.12/droid_lkm_pids.ko
# 脚本会自动拷到 dsm/ko/droid_lkm_pids.ko
```

只想重编它一个（省时间）：

```bash
docker run --rm \
  -e KDIR=/opt/ddk/kdir/android16-6.12 \
  -v "$PWD/droid_lkm":/src -w /src \
  docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
  sh -c 'make VER=android16-6.12 out/android16-6.12/droid_lkm_pids.ko'
```

它用到的源文件：

```
src/pidsmod/pids_main.c        显示层（两个打印函数 hook）
src/pidsmod/pids_ctl.c         功能层（pids.max + fork 拦截 + subtree_control 翻译）
src/compat/ds_compat.c
src/compat/ds_layout_anchor.c
+ DEPS_OBJS_ALL                 KallRecon / HooKern / KernCall / Type_info
```

> 功能层依赖 `register_kprobe()` 与 kallsyms 符号解析，不需要任何未导出符号。
> 曾经用过的 `copy_to_kernel_nofault` 在 OPPO 内核上未导出，已移除
> （改用直接 `memset` + per-CPU buffer）。若你自己加代码，先确认符号是否导出，
> 一个未导出符号会让整个模块 `insmod` 失败。

---

## 6. 手工加载（调试用，不依赖 KernelSU 自动化）

```sh
# 三个主模块
insmod droid_lkm.ko gate=0 mqueue=1 nspid=1 nspid_append=1 \
  skip_sysvipc=0 no_fake_ns=0 skip_do_exit=0
insmod droid_lkm_vendor.ko ghost=1 ghost_match=oplus_ pid_max_cap=0
insmod droid_lkm_misc.ko

# pids 模块（独立，可单独装卸，无参数）
insmod droid_lkm_pids.ko

# 网络模块（自解析地址，不传参）
insmod macvlan.ko
insmod ip6table_nat.ko
```

单独重装卸 pids（改代码后最常用的循环）：

```sh
rmmod droid_lkm_pids
insmod /path/to/droid_lkm_pids.ko
dmesg | grep droid_lkm_pids | tail -12
```

> ⚠ 重载会**清空 pids.max 表**（xarray 在模块退出时销毁）。测完需要重启容器，
> 让 Droidspaces 重新写入 `pids.max`。

卸载：

```sh
rmmod droid_lkm_pids        # 先卸它：它 hook 的打印函数被别人依赖的可能性最小
rmmod macvlan
rmmod ip6table_nat
rmmod droid_lkm_misc
rmmod droid_lkm_vendor
rmmod droid_lkm
```

---

## 7. 已知构建陷阱清单

| 陷阱 | 现象 | 修法 |
| --- | --- | --- |
| clang 版本不匹配 | `insmod` 时 `CFI failure` + 关机 | 必须用 DDK clang 19 |
| 漏 `-DCONFIG_MACVLAN=1` | `insmod` 时 `kernel BUG`（`if_macvlan.h:82`） | 加上宏 |
| shim 符号未改名 | 链接报 `Unknown symbol`（未导出符号） | 用 `-include` 宏改名，见 `macvlan_shim.h` |
| userns 里取错 `pt_regs` | 反复重启 | hook `ksys_unshare`，不要自己解析 `pt_regs` |
| devtmpfs 里 `dget(mnt_root)` | 空指针 panic | 只注册 filesystem，不起线程、不 hook |
| 脚本传入不存在的模块参数 | `insmod` 报 `Unknown parameter` | 删掉那些参数（`inline_hook` / `write_path` / `degrade`） |
| inline hook 打到 `cgroup_procs_write` | **加载后 22ms 整机断电** | trampoline 里是 `BRK #4`；该函数只能用 kprobe，且本项目已彻底移除该 hook |
| 用了未导出的内核符号 | `insmod` 报 `Unknown symbol` | 如 `copy_to_kernel_nofault`；改用自己的实现或直接写内存 |
| 在 `cgroup_post_fork` 读 `child->cgroups` | `pids.current` 恒为 0 | pre-handler 时还没赋值，改读 `kargs->cset` |
| 重复写 `cgroup.subtree_control` 的 `+memory` | Droidspaces 报 `Bad address` | memory enable 是粘性的，每个 cgroup 只替换一次 |
