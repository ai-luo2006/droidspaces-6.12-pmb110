> English version of [BUILD.md](../../src/BUILD.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# The Complete Build Flow from Source to .ko

This document records **all** steps from zero to producing the 6 ko files, reproducible.

---

## 0. Why You Must Use the Android DDK

The target kernel `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` has **KCFI**
enabled (clang's Control Flow Integrity). KCFI validates function signatures (type hash).
If any function prototype in the module does not match what the kernel was compiled with,
`insmod` immediately reports:

```
CFI failure at do_one_initcall (target: init_module+0x0 [macvlan])
```

and then **shuts down straight away**. This is not a soft failure, it is a hard panic.

Therefore:

- You must use the **clang that matches the target kernel** (clang 19 in this case).
- You must use the **KDIR** matching that kernel (`android16-6.12` in this case).
- A ko built with the system-provided clang 18 **will shut the device down on `insmod`**
  (verified the hard way, twice).

The DDK image provides both:

```
docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12
  KDIR = /opt/ddk/kdir/android16-6.12
  clang 19
```

---

## 1. Environment Requirements

| Component | Version / Source |
| --- | --- |
| Docker | Any working version (only used to run the DDK image) |
| Image | `docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12` |
| Compiler | clang 19 bundled in the image (**do not use the host clang**) |
| KDIR | `/opt/ddk/kdir/android16-6.12` (bundled in the image) |
| Cross toolchain | `aarch64-linux-gnu-` bundled in the image |
| Host kernel source | `android16-6.12` GKI branch commit `242e1a07f878` (optional, only for cross-checking / building your own DDK) |

Pull the image:

```bash
docker pull docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12
```

---

## 2. Building the Three Main Modules (droid_lkm / vendor / misc)

### 2.1 Fetching Third-Party Dependencies

The main modules depend on 4 third-party projects, with versions pinned in `droid_lkm/deps.lst`:

```
# <name> <rev>
KallRecon b258fb6
Type_info 7182dae
KernCall cdec722
HooKern 7a02afa
vendorcompat -
```

There is one more entry, `vendorcompat`, which ships inside the SDK and therefore has no rev
(it is written as `vendorcompat -` in `deps.lst`).

Fetch (requires KMSDK):

```bash
sh scripts/fetch-deps.sh
```

Or manually (equivalent):

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

The dependency tree ends up looking like this:

```
droid_lkm/deps/KallRecon/   (contains the full lib/ set of .c/.h/.S)
droid_lkm/deps/Type_info/
droid_lkm/deps/KernCall/
droid_lkm/deps/HooKern/
```

In addition, `deps.lst` contains one more entry, `vendorcompat -` (no rev, provided by the SDK itself).
It lives at:

```
droid_lkm/.sdk/builtin/vendorcompat/
```

The `droid_lkm/.sdk/builtin/` directory **is shipped with the package** (about 520 KB),
and `mk/deps.mk` automatically prefers `deps/<Name>/`, falling back to
`.sdk/builtin/<Name>/` when not found. So **a full offline build works**, no network needed.

> A missing `.sdk/builtin/vendorcompat/deps.mk` produces:
> `No rule to make target '/src/.sdk/builtin/vendorcompat/deps.mk'. Stop.`

### 2.2 Building with DDK

```bash
sh scripts/build-ddkk.sh android16-6.12
```

Internally the script is equivalent to:

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

Artifacts are in `droid_lkm/out/android16-6.12/`:

```
droid_lkm.ko
droid_lkm_vendor.ko
droid_lkm_misc.ko
```

> `VER` determines both the KMI selection and the output directory; it **must match the
> target kernel**.

### 2.3 Building Your Own DDK (Instead of Using the Ready-Made Image)

You first need to fetch the GKI source and build out `Module.symvers`:

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

Once you have `Module.symvers`, replace the DDK image's `/opt/ddk/kdir/android16-6.12`
with this kernel tree's `source + out`.

---

## 3. Building the Two Network Modules (macvlan / ip6table_nat)

These two **must** be built with DDK (same KCFI reason).

```bash
sh scripts/build-net-ddk.sh android16-6.12
```

Equivalent to:

```bash
for m in net_macvlan net_ip6nat; do
  docker run --rm \
    -e KDIR=/opt/ddk/kdir/android16-6.12 \
    -v "$PWD/$m":/src -w /src \
    docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
    sh -c 'make -C $KDIR M=/src clean >/dev/null 2>&1; make -C $KDIR M=/src modules'
done
```

> ⚠ You must use the **Kbuild** form `make -C $KDIR M=/src modules`.
> Running `make` directly in the source directory reports `No targets. Stop.`
> (because the Makefile is a Kbuild fragment with no standalone targets).

Actual output (macvlan):

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

Artifacts:

```
net_macvlan/macvlan.ko
net_ip6nat/ip6table_nat.ko
```

The script automatically copies them into `dsm/ko/`.

### 3.1 Key Build Switch for macvlan

`net_macvlan/Makefile`:

```make
obj-m := macvlan.o
macvlan-y := macvlan_core.o macvlan_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

# -include : redirect 7 unexported symbols
# -DCONFIG_MACVLAN=1 : make if_macvlan.h take the real implementation instead of the BUG() stub
CFLAGS_macvlan_core.o := -include $(src)/macvlan_shim.h -DCONFIG_MACVLAN=1
CFLAGS_macvlan_shim.o := -DCONFIG_MACVLAN=1

ccflags-y += -Wno-declaration-after-statement -Wno-unused-variable \
	-Wno-unused-function -Wno-strict-prototypes
ccflags-y += -I$(src)/kr
```

> ⚠ **`-DCONFIG_MACVLAN=1` is mandatory**. Without it, `linux/if_macvlan.h:82` hits the
> `BUG()` stub used when that config is not compiled in, and `insmod` immediately hits
> `kernel BUG`. This was the first-version pitfall of this project.

### 3.2 Key Points for ip6table_nat

`net_ip6nat/Makefile`:

```make
obj-m := ip6table_nat.o
ip6table_nat-y := ip6table_nat_core.o ip6t_nat_shim.o \
	kr/core.o kr/access.o kr/discover.o kr/symbol.o kr/slide.o \
	kr/fastboot.o kr/hint.o kr/variant.o kr/anchor.o

CFLAGS_ip6table_nat_core.o := -include $(src)/ip6t_nat_shim.h
ccflags-y += -I$(src)/kr
```

The fourth parameter of `ip6t_register_table()` must be
`const struct nf_hook_ops *` on 6.12; passing the wrong type compiles fine
but behaves incorrectly at runtime.

---

## 4. Packaging into a KernelSU-Installable zip

```bash
sh scripts/package-ksu-zip.sh
```

Artifact: `dist/Droidspaces-<version>.zip`; the zip root is directly the module content
(`module.prop` / `post-fs-data.sh` / `ko/` …), installable by KernelSU as-is.

The artifact layout is exactly the final installation directory:

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

After flashing, it lands in `/data/adb/modules/Droidspaces/`.

---

## 5. Building / Rebuilding `droid_lkm_pids`

The pids module is **produced together with §2** (it is already part of the `make` target
in `scripts/build-ddkk.sh`), so it usually needs no separate handling. You only need to
rebuild it after changing `src/pidsmod/`, which leaves the other five modules untouched:

```bash
sh scripts/build-ddkk.sh android16-6.12
# artifact: droid_lkm/out/android16-6.12/droid_lkm_pids.ko
# the script automatically copies it to dsm/ko/droid_lkm_pids.ko
```

To rebuild only this one (saves time):

```bash
docker run --rm \
  -e KDIR=/opt/ddk/kdir/android16-6.12 \
  -v "$PWD/droid_lkm":/src -w /src \
  docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
  sh -c 'make VER=android16-6.12 out/android16-6.12/droid_lkm_pids.ko'
```

Source files it uses:

```
src/pidsmod/pids_main.c        display layer (two print function hooks)
src/pidsmod/pids_ctl.c         function layer (pids.max + fork interception + subtree_control translation)
src/compat/ds_compat.c
src/compat/ds_layout_anchor.c
+ DEPS_OBJS_ALL                KallRecon / HooKern / KernCall / Type_info
```

> The function layer relies on `register_kprobe()` and kallsyms symbol resolution,
> and needs no unexported symbols. The previously used `copy_to_kernel_nofault` is
> unexported on OPPO kernels and has been removed (replaced with a direct `memset`
> plus a per-CPU buffer). If you add code yourself, check whether the symbol is exported
> first — a single unexported symbol will make the whole module fail `insmod`.

---

## 6. Manual Loading (for Debugging, Without KernelSU Automation)

```sh
# main three modules
insmod droid_lkm.ko gate=0 mqueue=1 nspid=1 nspid_append=1 \
  skip_sysvipc=0 no_fake_ns=0 skip_do_exit=0
insmod droid_lkm_vendor.ko ghost=1 ghost_match=oplus_ pid_max_cap=0
insmod droid_lkm_misc.ko

# pids module (standalone, can be loaded/unloaded individually, no parameters)
insmod droid_lkm_pids.ko

# network modules (self-resolving addresses, no parameters)
insmod macvlan.ko
insmod ip6table_nat.ko
```

Reloading pids alone (the most common loop after editing code):

```sh
rmmod droid_lkm_pids
insmod /path/to/droid_lkm_pids.ko
dmesg | grep droid_lkm_pids | tail -12
```

> ⚠ Reloading **clears the pids.max table** (the xarray is destroyed on module exit).
> After testing you need to restart the container so that Droidspaces rewrites `pids.max`.

Unloading:

```sh
rmmod droid_lkm_pids        # unload it first: the print functions it hooks are least likely to be depended on by others
rmmod macvlan
rmmod ip6table_nat
rmmod droid_lkm_misc
rmmod droid_lkm_vendor
rmmod droid_lkm
```

---

## 7. Known Build Pitfall Checklist

| Pitfall | Symptom | Fix |
| --- | --- | --- |
| clang version mismatch | `CFI failure` + shutdown on `insmod` | must use DDK clang 19 |
| missing `-DCONFIG_MACVLAN=1` | `kernel BUG` on `insmod` (`if_macvlan.h:82`) | add the macro |
| shim symbols not renamed | link reports `Unknown symbol` (unexported symbol) | rename via `-include` macro, see `macvlan_shim.h` |
| wrong `pt_regs` retrieval in userns | repeated reboots | hook `ksys_unshare`, do not parse `pt_regs` yourself |
| `dget(mnt_root)` in devtmpfs | null pointer panic | only register the filesystem, no thread, no hook |
| passing nonexistent module parameters via script | `insmod` reports `Unknown parameter` | delete those parameters (`inline_hook` / `write_path` / `degrade`) |
| inline hook applied to `cgroup_procs_write` | **full device power-off 22ms after loading** | the trampoline contains `BRK #4`; that function can only use kprobe, and this hook has been completely removed from the project |
| using an unexported kernel symbol | `insmod` reports `Unknown symbol` | e.g. `copy_to_kernel_nofault`; use your own implementation or write memory directly |
| reading `child->cgroups` in `cgroup_post_fork` | `pids.current` always 0 | it is not yet assigned in the pre-handler, read `kargs->cset` instead |
| repeatedly writing `+memory` to `cgroup.subtree_control` | Droidspaces reports `Bad address` | memory enable is sticky, replace only once per cgroup |
