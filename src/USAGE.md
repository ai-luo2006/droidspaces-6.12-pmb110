# 使用与排错

---

## 1. 安装

1. 把仓库里的 `dsm/` 目录（**整个目录**，含 `ko/`、`webroot/`、`desc.sh`）压成 zip。
   仓库里自带打包脚本，直接用它最省事：`sh scripts/package-ksu-zip.sh`。
   > KernelSU 的"从压缩包安装"要求 zip 根目录就是模块目录（即 zip 里直接是
   > `module.prop` / `post-fs-data.sh` / `ko/` …，不要多套一层文件夹）。
2. KernelSU 管理器 → 模块 → **从压缩包安装**，选这个 zip。
3. 重启。**首次不会自动加载任何 ko**，手机必然正常开机。

---

## 2. WebUI

KernelSU 管理器 → 模块 → Droidspaces → **WebUI**。三页：

| 页面 | 内容 |
| --- | --- |
| 检测 | 6 个模块各自的加载状态、开关（滑动）、内核/机型/守卫信息、加载 / 卸载按钮 |
| 容器 | 容器列表、运行状态、一键换源 / 网络优化 / 查看 sources.list |
| 日志 | `load.log` / `history.log` / dmesg 全文，导出、解阻断、一键诊断 |

顶部标题右侧的徽章是当前模块版本（读自 `module.prop`）。

### 「加载」按钮的行为

- 按「加载内容」里**开着的开关**执行：只开 3 个就只装 3 个。
- 6 个全关时按钮变灰，点了也不装任何东西。
- 开关只决定"装哪些"，**不会卸载已加载的**。要重载请先「卸载」再「加载」。
- 开关写的是 `skip_*` 文件，**下次加载才生效**；勾掉一个开关不会把已加载的模块卸掉。
- 「延迟加载」只对开机生效，手动点「加载」是立即执行。
- 「卸载」有二次确认弹窗，卸载顺序为 ip6nat → macvlan → pids → misc → vendor → lkm。

---

## 3. 开关（模块目录下的文件）

| 文件 | 作用 |
| --- | --- |
| `no_boot` | 开机不自动加载（**本包默认存在**） |
| `skip_lkm` | 跳过 `droid_lkm`（Docker 靠它，别关） |
| `skip_vendor` | 跳过 vendor 兼容层 |
| `skip_misc` | 跳过 misc（失去 userns + devtmpfs） |
| `skip_pids` | 跳过 pids（`--pids-limit` 失效，DS 的 Process limit 检查项变回 ✗） |
| `skip_macvlan` | 跳过 macvlan |
| `skip_ip6nat` | 跳过 ip6table_nat |
| `skip_net` | 一次跳过上面两个网络模块 |
| `no_log` | 关闭日志记录（不写 load.log / history.log，加载照常） |
| `no_precheck` | 关闭加载前自检 |
| `delay` | 内容为秒数，开机延迟加载（上限 600） |
| `alive_sec` | 存活窗口秒数，默认 120，上限 900 |
| `blocked` | 存在则一律不加载（守卫触发时生成） |
| `allow_any_device` | 跳过机型锁（**别随包发**） |
| `allow_any_kernel` | 跳过内核锁（**别随包发**） |
| `allow_list` | 追加机型白名单，一行一个代号（**别随包发**） |

---

## 4. Docker 用法

```sh
# 基本
docker run --rm hello-world

# 后台 + 端口 + 容器名
docker run -d --name deepseek_harness -p 8080:8080 <image>
docker ps          # 期望 Up (healthy)

# 允许沙箱 + 硬件访问（由 Droidspaces 侧开关控制）
# 在 Droidspaces App 里编辑容器，勾选「允许沙箱」「硬件访问」

# IPv6 NAT（需 ip6table_nat 已加载）
docker run --rm --net=nat <image>

# macvlan（需 macvlan 已加载；本机 WiFi 下数据面不通，见 VERIFY.md）
docker run --rm --net=macvlan <image>

# 进程数限制（需 droid_lkm_pids 已加载）
docker run --rm --pids-limit 256 hello-world
```

### `--pids-limit` 怎么给值

| 值 | 后果 |
| --- | --- |
| 未设 | 不启用限制（`pids.max` 保持 0） |
| < 68 | 容器自身起不来，`docker stop` 超时后被 SIGKILL |
| **≥ 256** | 推荐。空跑容器约需 68 个进程，256 留出余量 |
| `max` | 解析为不限制 |

注意点：

- 这是**模拟实现**（内核未编 `CONFIG_CGROUP_PIDS`）。通过写 `cgroup.procs`
  迁移进容器的进程不立即计入 `pids.current`，有 1 个的偏差。
- 想确认是不是真的在拦：

  ```sh
  su -c 'cat /sys/fs/cgroup/<容器目录>/pids.stats'
  # refused > 0 即生效
  ```

- 改完 limit 后**不要** `rmmod` 再 `insmod`：重载会清空 limit 表，
  需要重启容器让 Droidspaces 重新写 `pids.max`。
- 原理与完整限制见 [`docs/PIDS-ENFORCEMENT.md`](docs/PIDS-ENFORCEMENT.md)。

---

## 5. 排错

### App 显示"硬件访问 不可用"，但 `/proc/filesystems` 已有 devtmpfs

App 缓存问题，清掉即可：

```sh
su -c 'am force-stop com.droidspaces.app'
su -c 'sed -i -e "/name=\"host_capabilities\"/d" \
        -e "/name=\"host_capabilities_boot_id\"/d" \
        /data/data/com.droidspaces.app/shared_prefs/droidspaces_prefs.xml'
```

### 点「加载」没反应 / 只装了一部分

看「日志」页输出，确认「加载内容」里对应开关是打开的。
若提示"机型不符 / 内核不符"，说明被硬锁拦了。

### 守卫显示"崩溃阻断"

上次加载后没撑过存活窗口。先「解阻断」，再手动「加载」，观察 10 分钟；
确认稳定后再开自启。反复出现就把 `load.log` 和 `history.log` 发出来。

### 提示 `Unknown symbol`

用了非 DDK 工具链编译的 ko。必须用 clang 19 + 匹配 KDIR 重编
（见 `BUILD.md`）。

### 提示 `CFI failure` 然后关机

同上是 KCFI 不匹配。用系统 clang 18 编的 ko 就是这个下场。

### `insmod` 后关机，pstore 里有记录

看 `/sys/fs/pstore/` 下的崩溃日志。若提到 `if_macvlan.h` → 漏了
`-DCONFIG_MACVLAN=1`；若提到 `CFI` → 工具链不匹配。

---

## 6. 恢复（bootloop / 变砖）

按顺序尝试：

1. **KernelSU 安全模式**：开机时按住音量下，KernelSU 会停用所有模块。
2. **adb / recovery 删除**：
   ```sh
   adb shell su -c 'rm -rf /data/adb/modules/Droidspaces'
   ```
3. **删除 blocked 后重启**（会重试一次加载）：
   ```sh
   adb shell su -c 'rm -f /data/adb/modules/Droidspaces/blocked'
   ```

事后务必：先手动加载验证 10 分钟，再考虑打开"开机自动加载"。

---

## 7. 分享给别人之前

- 只给**同机型（PMB110）、同内核（6.12.69）**的人。机型/内核锁会自动拦其它设备。
- **不要**附带 `allow_any_device` / `allow_any_kernel` / `allow_list`。
- 提醒对方：装完先别开自启，手动点「加载」，正常用 10 分钟再开。
- 附上 `README.md` 的「硬性限制」和「已知限制」两节。
