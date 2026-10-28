> English version of [USAGE.md](../../src/USAGE.md). Droidspaces 6.12 — v1.0.20-6.12 (author: ai-luo).

# Usage and Troubleshooting

---

## 1. Installation

1. Zip up the `dsm/` directory from the repository (**the whole directory**, including `ko/`, `webroot/`, `desc.sh`).
   > KernelSU's "Install from zip" requires the zip root to be the module directory itself (that is, the zip
   > directly contains `module.prop` / `post-fs-data.sh` / `ko/` … with no extra wrapping folder).
   >
   > The repo ships its own packaging script, so you can just run
   > `sh scripts/package-ksu-zip.sh` instead of zipping it by hand.
2. KernelSU Manager → Modules → **Install from zip**, pick this zip.
3. Reboot. **No ko is auto-loaded on the first boot**, so the phone is guaranteed to boot normally.

---

## 2. WebUI

KernelSU Manager → Modules → Droidspaces → **WebUI**. Three pages:

| Page | Content |
| --- | --- |
| Detection | load status of each of the 6 modules, switches (sliders), kernel/device/guard information, load / unload buttons |
| Containers | container list, running status, one-tap source switch / network optimization / view sources.list |
| Logs | full text of `load.log` / `history.log` / dmesg, export, unblock, one-tap diagnostics |

The badge at the right of the top title is the current module version (read from `module.prop`).

### Behavior of the "Load" button

- It follows the switches that are **on** under "Load content": turn on only 3 and only 3 get installed.
- When all 6 are off, the button turns grey and tapping it installs nothing.
- The switches only decide "which ones to install"; they **never unload something already loaded**.
  To reload, tap "Unload" first and then "Load".
- The switches write the `skip_*` files and **take effect on the next load**; unchecking a switch does not
  unload an already loaded module.
- "Delayed load" only applies at boot; tapping "Load" manually executes immediately.
- "Unload" has a confirmation dialog, and the unload order is ip6nat → macvlan → pids → misc → vendor → lkm.

---

## 3. Switches (files in the module directory)

| File | Purpose |
| --- | --- |
| `no_boot` | do not auto-load on boot (**present in this package by default**) |
| `skip_lkm` | skip `droid_lkm` (Docker depends on it, do not turn it off) |
| `skip_vendor` | skip the vendor compatibility layer |
| `skip_misc` | skip misc (loses userns + devtmpfs) |
| `skip_pids` | skip pids (`--pids-limit` stops working, the DS Process limit check reverts to ✗) |
| `skip_macvlan` | skip macvlan |
| `skip_ip6nat` | skip ip6table_nat |
| `skip_net` | skip both network modules above at once |
| `no_log` | disable logging (load.log / history.log are not written, loading still works) |
| `no_precheck` | disable the pre-load self-check |
| `delay` | content is a number of seconds; delays loading at boot (max 600) |
| `alive_sec` | survival window in seconds, default 120, max 900 |
| `blocked` | if present, nothing is ever loaded (created when a guard triggers) |
| `allow_any_device` | skip the device model lock (**do not ship with the package**) |
| `allow_any_kernel` | skip the kernel lock (**do not ship with the package**) |
| `allow_list` | append device models to the whitelist, one codename per line (**do not ship with the package**) |

---

## 4. Docker usage

```sh
# basics
docker run --rm hello-world

# background + port + container name
docker run -d --name deepseek_harness -p 8080:8080 <image>
docker ps          # expect Up (healthy)

# allow sandbox + hardware access (controlled by switches on the Droidspaces side)
# In the Droidspaces app, edit the container and tick "Allow sandbox" and "Hardware access"

# IPv6 NAT (requires ip6table_nat to be loaded)
docker run --rm --net=nat <image>

# macvlan (requires macvlan to be loaded; the data plane does not work over local WiFi, see VERIFY.md)
docker run --rm --net=macvlan <image>

# process count limit (requires droid_lkm_pids to be loaded)
docker run --rm --pids-limit 256 hello-world
```

### How to pick a value for `--pids-limit`

| Value | Consequence |
| --- | --- |
| not set | no limit is enabled (`pids.max` stays 0) |
| < 68 | the container cannot even start itself; `docker stop` times out and then it is SIGKILLed |
| **≥ 256** | recommended. An idle container needs about 68 processes, 256 leaves headroom |
| `max` | parsed as unlimited |

Things to watch out for:

- This is a **simulated implementation** (the kernel was not built with `CONFIG_CGROUP_PIDS`). Processes
  migrated into the container by writing `cgroup.procs` are not counted in `pids.current` immediately,
  so there is an off-by-one deviation.
- To confirm that it is really enforcing:

  ```sh
  su -c 'cat /sys/fs/cgroup/<container dir>/pids.stats'
  # refused > 0 means it is in effect
  ```

- After changing the limit, do **not** `rmmod` and `insmod` again: reloading clears the limit table,
  and the container has to be restarted so Droidspaces writes `pids.max` again.
- Principles and the full list of limitations are in [`PIDS-ENFORCEMENT.md`](PIDS-ENFORCEMENT.md).

---

## 5. Troubleshooting

### The app shows "Hardware access unavailable", but `/proc/filesystems` already lists devtmpfs

This is an app cache issue; just clear it:

```sh
su -c 'am force-stop com.droidspaces.app'
su -c 'sed -i -e "/name=\"host_capabilities\"/d" \
        -e "/name=\"host_capabilities_boot_id\"/d" \
        /data/data/com.droidspaces.app/shared_prefs/droidspaces_prefs.xml'
```

### Tapping "Load" does nothing / only part of the modules got installed

Check the output on the "Logs" page and confirm that the corresponding switch under "Load content" is on.
If it reports "device mismatch / kernel mismatch", you are being blocked by the hard locks.

### The guard shows "crash block"

The previous load did not survive the survival window. First "Unblock", then "Load" manually and observe
for 10 minutes; once it is confirmed stable, enable autostart. If it keeps happening, send out
`load.log` and `history.log`.

### `Unknown symbol` is reported

You used a ko built with a non-DDK toolchain. You must rebuild with clang 19 and a matching KDIR
(see `BUILD.md`).

### `CFI failure` is reported and then the phone shuts down

Same root cause: KCFI mismatch. A ko built with the system clang 18 ends exactly like this.

### The phone shuts down after `insmod` and pstore has a record

Look at the crash logs under `/sys/fs/pstore/`. If they mention `if_macvlan.h`, you are missing
`-DCONFIG_MACVLAN=1`; if they mention `CFI`, the toolchain does not match.

---

## 6. Recovery (bootloop / brick)

Try in order:

1. **KernelSU safe mode**: hold volume down while booting; KernelSU disables all modules.
2. **Delete via adb / recovery**:
   ```sh
   adb shell su -c 'rm -rf /data/adb/modules/Droidspaces'
   ```
3. **Delete `blocked` and reboot** (it will retry loading once):
   ```sh
   adb shell su -c 'rm -f /data/adb/modules/Droidspaces/blocked'
   ```

Afterwards, be sure to: load manually and verify for 10 minutes first, and only then consider turning on
"Load on boot".

---

## 7. Before sharing it with others

- Only give it to people with the **same device (PMB110) and the same kernel (6.12.69)**. The device and
  kernel locks block other devices automatically.
- **Do not** include `allow_any_device` / `allow_any_kernel` / `allow_list`.
- Remind them: do not enable autostart right after installing; tap "Load" manually, use it normally for
  10 minutes, and only then turn autostart on.
- Attach the "Hard requirements" and "Known limitations" sections of `README.md`.
