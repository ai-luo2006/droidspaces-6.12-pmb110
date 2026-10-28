> English version of [README.md](../README.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# Droidspaces Kernel Module Suite (OnePlus Ace 6 Ultimate Edition / PMB110 / Android 16 / 6.12.69)

Six LKM kernel modules loaded via KernelSU, plus full source code and the complete build / deploy / verify procedure.

Current version **v1.0.20-6.12** (`dsm/module.prop`, shown directly at the right of the WebUI title), author `ai-luo`.
The documentation comes in two languages: the root directory holds the Chinese version, and [`./`](./) (i.e. `docs/en/`) holds the matching English version.

Goal: **without flashing a kernel**, backfill the options missing for Docker / container runtimes on the
stock GKI kernel — `CONFIG_PID_NS` / `CONFIG_IPC_NS` / `CONFIG_SYSVIPC` / `CONFIG_POSIX_MQUEUE` /
`CONFIG_DEVTMPFS` — and additionally provide macvlan and IPv6 NAT.

> **On `CONFIG_USER_NS`**: this one **cannot** be backfilled. The stock kernel was built without it
> (`unshare -U true` returns EINVAL, `/proc/<pid>/ns/user` does not exist). The module only performs
> id translation at syscall boundaries, which is **not isolation** — container root is host root.
> See the evidence chain at the end of [`CHANGELOG.md`](../CHANGELOG.md).

---

## 0. One-minute overview

| Module | Purpose | Status |
| --- | --- | --- |
| `droid_lkm.ko` | PID/IPC namespace, System V IPC, POSIX mqueue | ✅ loads successfully |
| `droid_lkm_vendor.ko` | oplus vendor compatibility (fingerprint / parameter fixes) | ✅ loads successfully |
| `droid_lkm_misc.ko` | devtmpfs (hardware access works); user-namespace view (**inactive: kernel built without `CONFIG_USER_NS`**) | ✅ loads successfully |
| `droid_lkm_pids.ko` | `pids` controller: display layer (check item turns green) + **enforcement layer** (`--pids-limit` really blocks fork), can be unloaded on its own | ✅ verified on a real device (all 40 DS checks green, `refused` > 0) |
| `macvlan.ko` | dedicated LAN IP for containers (`docker run --net=macvlan`) | ⚠ control plane usable, data plane limited by WiFi |
| `ip6table_nat.ko` | IPv6 NAT (IPv6 for `docker run --net=nat`) | ✅ loads successfully, rules can be added and removed |

Tested combination (the **only verified combination**):

- Device: OnePlus Ace 6 Ultimate Edition, `PMB110`
- System: `17.0.0.101(CNO1C10P02)`
- Kernel: `6.12.69-android16-6-g242e1a07f878-ab15865446-4k`

---

## 1. Directory layout

```
.
├── README.md                  this file
├── BUILD.md                   full build procedure from source to ko
├── VERIFY.md                  full post-deployment verification checklist (with pass criteria)
├── USAGE.md                   usage and troubleshooting (WebUI / Docker / recovery)
├── LICENSE                    GPL-2.0 (code of this project)
├── THIRD-PARTY.md             third-party source attribution and licenses
├── docs/
│   ├── PRINCIPLE.md           implementation principles (what each module actually does)
│   ├── PIDS-ENFORCEMENT.md    pids enforcement layer: how `pids.max` is built, how fork is blocked
│   ├── BUILD-FOR-OTHERS.md    building for someone else's phone (porting procedure)
│   └── en/                    English versions of the documents above
├── droid_lkm/                 main modules (droid_lkm / vendor / misc)
│   ├── src/
│   │   ├── main/              PID_NS / IPC_NS / SysV / mqueue / slot
│   │   ├── compat/            vendor compatibility, ghost task, ddl history table
│   │   ├── hook/              inline hook wrapper
│   │   ├── misc/              userns, devtmpfs, xtables extensions
│   │   ├── pidsmod/           standalone pids controller name reporting module
│   │   └── core/              kallsyms symbol resolution, Type-info, caps
│   ├── deps/                  third-party dependency source snapshot (offline build possible)
│   ├── mk/                    dependency assembly makefile fragments
│   ├── Makefile               Kbuild file for the three main modules
│   └── deps.lst               third-party dependencies and pinned versions
├── net_macvlan/               macvlan.ko source (7-symbol shim + KallRecon)
├── net_ip6nat/                ip6table_nat.ko source (9-symbol shim + KallRecon)
├── scripts/
│   ├── build-ddkk.sh          builds the three main modules (DDK)
│   ├── build-net-ddk.sh       builds macvlan / ip6table_nat (DDK, required)
│   ├── fetch-deps.sh          fetches third-party dependencies
│   ├── package-ksu-zip.sh     packs a KernelSU-installable zip
│   ├── load_macvlan.sh        manual single-module load
│   └── load_ip6nat.sh         manual single-module load
├── dsm-pids/                  **minimal package that installs pids only** (standalone ko + manual load; for users who only need --pids-limit)
├── dsm/                       the KernelSU module itself (i.e. the final installed directory layout)
│   ├── module.prop
│   ├── post-fs-data.sh        guards / switches / load logic
│   ├── service.sh             survival guard
│   ├── uninstall.sh
│   ├── desc.sh                writes "Loaded N/6: ..." back into the description of module.prop
│   ├── ko/*.ko                6 prebuilt modules
│   └── webroot/index.html     three-page WebUI (status / containers / logs)
├── prebuilt/                  checksums and notes for prebuilt artifacts (the ko files live in dsm/ko/)
└── docs/                      implementation principles + guide to building for others
```

---

## 2. Get started in five minutes

```bash
# 1. Zip up the whole dsm/ directory and flash it with KernelSU ("Install from zip").
#    Note: no_boot is pre-baked into the package, so the first install will not
#    auto-load and the phone is guaranteed to boot normally.

# 2. KernelSU Manager -> Modules -> Droidspaces -> WebUI
#    Tap "Load" manually. Always do it manually the first time; do not enable autostart.

# 3. Confirm that all six modules show "loaded"
su -c 'cat /proc/modules'

# 3.5 The module list shows directly what got loaded (maintained automatically by desc.sh, no manual edits)
#    The description of KernelSU -> Modules -> Droidspaces becomes:
#      Loaded 4/6: lkm · vendor · misc · pids
#    When nothing is loaded, the original description text is shown.
#    The module page in the manager needs a pull-to-refresh before the update is visible.
#    The "Module description" row on the WebUI status page reads the same field.

# 4. Open Droidspaces, edit the container, confirm that "Allow sandbox" and "Hardware access" can be turned on

# 5. Process count limit (requires droid_lkm_pids to be loaded; >= 256 recommended)
docker run --rm --pids-limit 256 hello-world
su -c 'cat /sys/fs/cgroup/<container dir>/pids.stats'   # refused > 0 means it is really enforcing

# 6. If everything is fine for 10 minutes, go back to the WebUI and turn on "Load on boot"
```

If you only want the pids module, use the minimal package `dsm-pids/`; see its `README.txt`.

Full verification is in `VERIFY.md`; the full recovery procedure is in `USAGE.md`.

### Using it on someone else's device / building for someone else

`dsm/ko/*.ko` (prebuilt artifacts) has **only been verified on PMB110 / 6.12.69**; others cannot use it directly.
A different device or a different kernel requires a rebuild. The full porting procedure is in
[`BUILD-FOR-OTHERS.md`](BUILD-FOR-OTHERS.md). The one-liner version:

```bash
# Use the DDK target matching the GKI version of the other party's kernel
sh scripts/build-ddkk.sh    android15-6.6     # e.g.: 6.6 kernel
sh scripts/build-net-ddk.sh android15-6.6
sh scripts/package-ksu-zip.sh
# After installing, drop in the device model lock
su -c 'echo "<target device codename>" >> /data/adb/modules/Droidspaces/allow_list'
```

**Key constraint**: the clang version must match the one used to build the target kernel (KCFI).
A module built with the system clang 18 will shut the phone down the moment you `insmod` it. You must use the DDK image.

---

## 3. Hard requirements (read this)

| Item | Requirement |
| --- | --- |
| Device | `ro.product.*` contains `PMB110` |
| Kernel | `6.12.69-*` |
| Bootloader/root | KernelSU (not Magisk) |

If any of them does not match, `post-fs-data.sh` exits immediately and **not a single ko is loaded**.

Same device but a different system version = **unverified**; the kernel code layout may already have changed.
If you really want to try it on another machine:

```bash
touch /data/adb/modules/Droidspaces/allow_any_device   # skip the device model lock
touch /data/adb/modules/Droidspaces/allow_any_kernel   # skip the kernel lock
echo  <codename> >> /data/adb/modules/Droidspaces/allow_list # append to the whitelist
```

> ⚠ These three "unlock" files must **not be shipped with the package**.

---

## 4. Safety mechanisms (bootloop prevention)

1. Autostart is **off by default** on first install / after an upgrade (`no_boot` is pre-baked into the package).
2. Device model + kernel **double lock**; exit immediately on mismatch.
3. **Bootloop guard**: `insmod.inflight` is dropped before `insmod` and deleted on success; if some boot
   crashes inside `insmod`, the next boot detects it, creates `blocked`, and skips loading.
4. **Survival guard**: after a successful `insmod`, `boot_pending` is kept; `service.sh` waits 120 seconds
   to confirm the system is still alive before writing `.boot_ok`. On the next boot, if `boot_pending`
   still exists and `.boot_ok` does not, it is judged as "crashed after loading" and `blocked` is created
   automatically to skip loading. The window can be tuned with `alive_sec` (default 120, max 900).
5. `misc` by default **only registers the devtmpfs filesystem**: it starts no threads and touches no dentries,
   so it cannot crash.
6. `droid_lkm_pids` is a **standalone module** with two layers; both can be `rmmod`ed separately and it can
   coexist with an already loaded `droid_lkm_misc`:
   - **Display layer**: hooks the two print functions `cgroup_controllers_show` / `proc_cgroupstats_show`
     so that the Droidspaces `Process limit support` check item shows as passed.
   - **Enforcement layer**: exposes `pids.max` / `pids.current` / `pids.stats`, and uses a kprobe on
     `cgroup_can_fork` to make an over-limit fork return `-EAGAIN`, so that `--pids-limit` really takes effect.
   Dropping `skip_pids` keeps it from being loaded. Technical details are in
   [`PIDS-ENFORCEMENT.md`](PIDS-ENFORCEMENT.md).
7. The `release` parameter of `droid_lkm_misc` is bidirectional:
   `echo 1 > .../parameters/release` releases the internal devtmpfs mount (must be done before unloading,
   otherwise `rmmod` reports EBUSY); `echo 0 > .../parameters/release` puts it back
   (after being released, the devtmpfs filesystem is unregistered, and this is what restores it).

Rescue order: KernelSU safe mode (hold volume down while booting) → or delete `/data/adb/modules/Droidspaces`
→ or delete `blocked` and reboot (it will retry once).

---

## 5. Switch reference (files in the module directory)

| File | Purpose |
| --- | --- |
| `no_boot` | do not auto-load on boot (**present in this package by default**) |
| `skip_lkm` | skip `droid_lkm` (Docker depends on it, do not turn it off) |
| `skip_vendor` | skip the vendor compatibility layer |
| `skip_misc` | skip misc (loses userns + devtmpfs) |
| `skip_pids` | skip the pids reporting module (the DS Process limit check reverts to ✗) |
| `skip_macvlan` | skip macvlan |
| `skip_ip6nat` | skip ip6table_nat |
| `skip_net` | skip both network modules above at once |
| `no_log` | disable logging |
| `no_precheck` | disable the pre-load self-check |
| `delay` | content is a number of seconds; delays loading at boot (max 600) |
| `alive_sec` | survival window in seconds (default 120, max 900) |
| `blocked` | if present, nothing is ever loaded (created when a guard triggers) |

---

## 6. Known limitations

- **The `pids` controller is a simulated implementation**, not the native kernel `CONFIG_CGROUP_PIDS`:
  - Processes migrated into a container by writing `cgroup.procs` are not counted in `pids.current`
    immediately (a real controller would count them), so there is an off-by-one deviation;
  - `--pids-limit` should be **≥ 256** (an idle container itself needs about 68 processes; setting 32
    starves the container, which shows up as `docker stop` timing out and then being SIGKILLed);
  - Changing the kernel requires a rebuild; if the layout self-check fails, the enforcement layer
    automatically degrades to a pure display layer (it will not crash, it just will not enforce).
  See [`PIDS-ENFORCEMENT.md`](PIDS-ENFORCEMENT.md#8-known-deviations-and-limitations) for the full description.
- **macvlan data plane does not work**: the local WiFi driver accepts only one MAC address; the interface
  can be created and deleted, but the TX counter grows while RX stays 0 and ARP is always FAILED.
  Using it for real requires a USB wired Ethernet adapter. macvlan is **not needed** in host network mode.
- **KCFI is strict**: any rebuild must use the matching DDK clang 19; a module built with the system
  clang 18 will shut the phone down on `insmod`.

---

## 7. License

Code owned by this project is released under **GPL-2.0**. The imported third-party sources (KallRecon,
HooKern, KernCall, Type_info, the kernel macvlan / ip6table_nat implementations) each keep their
original license; see `THIRD-PARTY.md` for details.

`macvlan_core.c` and `ip6table_nat_core.c` are derived from the Linux kernel source,
Copyright Patrick McHardy et al., SPDX GPL-2.0-or-later / GPL-2.0-only.
