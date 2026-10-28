> English version of [BUILD-FOR-OTHERS.md](../BUILD-FOR-OTHERS.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# Building for Someone Else's Phone (Adaptation Procedure)

This section answers the question: "I gave this repository to a friend — how does he build a working
ko for his own phone?"

---

## 0. The one-sentence conclusion

**The build must use the kernel from "his own phone" as KDIR, and must use the clang that matches that
kernel.** The ready-made `prebuilt/*.ko` in this repository have only been verified on PMB110 /
6.12.69; others **cannot use them directly**.

---

## 1. First, find out the 3 parameters of the other phone

Run this on the other person's phone (already rooted):

```sh
uname -r                                   # kernel version string
getprop ro.build.version.release           # Android major version
getprop ro.product.device                  # device codename
getprop ro.product.model                   # device model name
cat /proc/version                          # detailed compiler version
```

Measured values on this device (OnePlus Ace 6 Ultimate Edition):

```
uname -r  = 6.12.69-android16-6-g242e1a07f878-ab15865446-4k
release   = 17
device    = PMB110
model     = OnePlus Ace 6 Ultimate Edition
```

---

## 2. Deciding which route to take

### Route A: same device model as the verified combination → use the prebuilt ko directly

You may use the ko files in `prebuilt/` only if all of the following conditions are met:

- the device codename contains `PMB110`
- the kernel `uname -r` starts with `6.12.69-`

If so: just zip up `dsm/` and install it — no compilation is needed.

### Route B: same kernel line, different device model → rebuild the main modules + relax the device lock

The kernel is still `6.12.69-*` (or another GKI 6.12), only the device model differs:

```bash
# 1. Build with the same DDK image (same kernel version → same build parameters)
sh scripts/build-ddkk.sh android16-6.12
sh scripts/build-net-ddk.sh android16-6.12

# 2. Package
sh scripts/package-ksu-zip.sh

# 3. After installing, relax the device lock (inside the module directory)
su -c 'echo "<target device codename>" >> /data/adb/modules/Droidspaces/allow_list'
```

> `allow_list` supports **substring** matching, so filling in a prefix such as `PMB110` also works.
> What is matched is the concatenated fingerprint of `ro.product.device / model / board /
> odm.device / vendor.device / boot.product.hardware.sku / device-tree model`.

### Route C: different kernel version (6.6 / 6.1 / 5.15 …) → switch DDK target and rebuild

DDK images are tagged by Android + kernel version; one tag corresponds to one KDIR:

| DDK tag | KDIR kernel version string | Android |
| --- | --- | --- |
| `android17-6.18` | 6.18.x | Android 17 |
| `android16-6.12` | 6.12.x | Android 16 |
| `android15-6.6` | `6.6.127-4k-g46a034eca005` | Android 15 |
| `android14-6.1` | `6.1.166` | Android 14 |
| `android14-5.15` | 5.15.x | Android 14 |
| `android13-5.15` | `5.15.202-android13-5.15.202_r00` | Android 13 |
| `android13-5.10` | 5.10.x | Android 13 |
| `android12-5.10` | 5.10.x | Android 12 |

The `x` here means your own device does not have to carry the exact same kernel revision: the kernel
bundled in the DDK image may be 6.12.76, while this repo's target device PMB110 runs 6.12.69 — both
belong to the same tag `android16-6.12`.

Usage:

```bash
sh scripts/build-ddkk.sh    android15-6.6
sh scripts/build-net-ddk.sh android15-6.6
sh scripts/package-ksu-zip.sh
```

After switching the target, also note:

1. **The module source has version branches.** `droid_lkm/Makefile` contains a `VENDOR_VER` check
   (only 6.6 / 6.12 supported), and it picks the `munmap` implementation unit based on whether
   certain symbols exist in `System.map` (`munmap_vmi.o` / `munmap_mas.o` / `munmap_legacy.o`).
   This logic is automatic and needs no manual changes.
2. **The shim symbols for the network modules may change.** The sets of unexported macvlan /
   ip6tables symbols on 6.12 and 6.6 are not exactly the same. If the build reports
   `Unknown symbol`, that kernel version needs a different shim list, which must be filled in
   according to the error message.
3. **The vermagic must match.** If it cannot be loaded, first check:
   ```sh
   strings /data/adb/modules/Droidspaces/ko/droid_lkm.ko | grep vermagic
   ```
   It must match the `major.minor.patch` of `uname -r`.

### Route D: heavily vendor-customized kernels (the most troublesome)

Some devices (such as this OnePlus/OPPO device) do not run a pure GKI kernel but one heavily modified
by the vendor:

- the kernel version string carries a suffix like `-android16-6-g242e1a07f878-ab15865446-4k`
- the vendor merged its own patches and changed structure layouts
- it may use a non-public clang version

In this case you **must build KDIR yourself from the kernel source tree for that device**:

```bash
# 1. Obtain the kernel source for that device (vendor open-source repo / kernel source zip)
git clone <kernel repo for that device> ksrc && cd ksrc

# 2. Use the clang that matches that kernel (see the clang version in /proc/version)
#    Android 16 GKI uses clang 19; a version mismatch causes KCFI failure

# 3. Build KDIR
make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- gki_defconfig
scripts/config --file .config --enable CONFIG_USER_NS
make LLVM=1 ARCH=arm64 olddefconfig
make -j$(nproc) LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image
make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
```

Once you have KDIR, use this repository's build scripts with KDIR replaced:

```bash
docker run --rm \
  -e KDIR=/your/kdir \
  -v "$PWD/droid_lkm":/src -w /src \
  docker.cnb.cool/ylarod/ddk/ddk-min:android16-6.12 \
  sh -c 'make -C $KDIR M=/src modules'
```

---

## 3. What "KCFI must match" actually means

The target kernel has **KCFI** enabled (clang Control Flow Integrity). Every function signature is
hashed into the kernel, and at module load time the kernel verifies the signature hash of every
function in the module.

- Signature mismatch → `CFI failure at do_one_initcall (target: init_module+0x0 [xxx])`
- and then the **kernel shuts the device down directly** (it does not return an error).

Therefore:

| Item | Requirement |
| --- | --- |
| clang version | Must match the clang that built that kernel (6.12 GKI → clang 19) |
| KDIR | Must be the build output of that kernel (including `Module.symvers`) |
| Function prototypes | Consistent with the kernel headers (this project uses an `-include` shim to force the rename, so the compiler validates them) |

**A module built with the system's own clang 18 will shut the device down on `insmod`** — this project
hit that twice in real testing. The only reason the DDK image exists is to provide a matching
clang + KDIR combination.

---

## 4. Post-installation self-check list

Verify in order on the other person's phone; if any step fails, stop and investigate:

```sh
# ① all six modules present
grep -E '^(droid_lkm|droid_lkm_vendor|droid_lkm_misc|droid_lkm_pids|macvlan|ip6table_nat) ' /proc/modules

# ② no Unknown symbol / CFI failure
dmesg | grep -iE 'Unknown symbol|CFI failure' | tail

# ③ user namespace available (prerequisite for "allow sandbox")
cat /proc/self/uid_map | head -1     # expect "0 0 4294967295"

# ④ devtmpfs registered
grep devtmpfs /proc/filesystems        # expect "nodev  devtmpfs"

# ⑤ Docker runs
docker run --rm hello-world

# ⑥ IPv6 NAT
ip6tables -t nat -L -n
```

---

## 5. Common failures and corresponding actions

| Symptom | Cause | Action |
| --- | --- | --- |
| `insmod: ERROR: could not insert module ... Invalid module format` | vermagic mismatch | Switch to the DDK target for the corresponding kernel version and rebuild |
| `Unknown symbol xxx` | The set of unexported symbols differs on that kernel version | Extend the shim list; confirm the `CONFIG_TRIM_UNUSED_KSYMS` situation |
| Shutdown after `CFI failure` | clang version mismatch | You must use the DDK image's clang 19 |
| `kernel BUG` at `if_macvlan.h` | `-DCONFIG_MACVLAN=1` missing | It is already in the Makefile; check whether it was modified |
| Script reports "kernel mismatch" | The kernel version is not in `6.12.69-*` | `touch allow_any_kernel`, or modify the case branch in `post-fs-data.sh` |
| Script reports "device mismatch" | The device model is not in the allowlist | `echo <codename> >> allow_list` |
| Boot loop after installation | The crash guard did not take effect | See the recovery section of [`USAGE.md`](USAGE.md) |

---

## 6. On whether to relax the "kernel guard"

The guards in `post-fs-data.sh` come in two layers:

```sh
# kernel layer
case "$REL" in
  6.12.69-*) : ;;                  # only 6.12.69 is allowed through
  *) exit 0 ;;                     # everything else is not loaded
esac

# device layer
case "$FINGER" in *PMB110*) ok=1 ;; esac
```

For a package given to someone else, the recommended handling is:

- **Keep the kernel lock.** A wrong kernel version shuts the device down directly; the risk is too
  great.
- **Turn the device lock into an allowlist** and let the other party add their own codename:
  ```sh
  # remove the PMB110 hard match and read allow_list instead
  # or simply write the other party's codename into allow_list
  ```

If you really must relax the kernel lock, you **must** first change the `case` in `post-fs-data.sh`
into a wildcard such as `6.12.*` / `6.6.*`, rather than having the other party create
`allow_any_kernel` — the latter disables the only life-saving gate.
