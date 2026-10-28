# ① 安装与首次验证

目标：装完、跑通、确认手机没被弄坏。**全程约 10 分钟。**

---

## 0. 先确认前提

```sh
su -c 'uname -r'
```

应该输出 `6.12.69-android16-6-g242e1a07f878-ab15865446-4k` 之类。

- 内核是 `6.12.69-*` ✓ 继续
- 是别的版本 → **停**。模块自带机型锁会拒绝加载，别硬来

还要确认：

```sh
su -c 'ls /data/local/Droidspaces/bin/droidspaces && echo "Droidspaces OK"'
```

## 1. 装模块

**KernelSU → 模块 → 安装 → 选 zip → 重启**

zip 从哪来：

| 来源 | 地址 |
|---|---|
| Release（推荐） | https://github.com/ai-luo2006/droidspaces-6.12-pmb110/releases |
| 仓库文件 | `release/v1.0.20/Droidspaces-v1.0.20-6.12.zip` |

装完**先别急着开容器**。首次安装会自动放一个 `no_boot` 文件，意思是"开机不自动加载模块"，
这是故意的 —— 让你先手动验证一次，确认手机正常再开自启。

## 2. 手动加载

重启后在 Termux 里：

```sh
su -c 'PH_ACTION=1 sh /data/adb/modules/Droidspaces/post-fs-data.sh'
```

预期输出：

```
机型 PMB110 (OP64D7L1) 校验通过
系统 PMB110_17.0.0.101(CN01) / 内核 6.12.69-...
自检通过
droid_lkm 加载成功
droid_lkm_vendor 加载成功
droid_lkm_misc 加载成功
droid_lkm_pids 加载成功
macvlan 加载成功
ip6table_nat 加载成功
完成: 4 个 droid 模块在线
```

（`macvlan` / `ip6table_nat` 是 net 模块，不算在"4 个 droid 模块"里，正常。）

## 3. 验证六件事

一条命令全查完：

```sh
su -c 'echo "--- 模块 ---"; grep "^droid_lkm" /proc/modules
echo "--- 内核 ---"; uname -r
echo "--- 版本 ---"; grep -m1 ^version= /data/adb/modules/Droidspaces/module.prop
echo "--- adopt 开关（必须是 N）---"; cat /sys/module/droid_lkm_misc/parameters/userns_adopt 2>/dev/null
echo "--- unshare 探测 ---"; unshare -U true; echo "rc=$?"'
```

| # | 检查 | 期望 |
|---|---|---|
| 1 | `droid_lkm` 在 `/proc/modules` 里 | 有，refcount ≥ 1 |
| 2 | 内核版本 | `6.12.69-*` |
| 3 | 模块版本 | `v1.0.20-6.12` |
| 4 | `userns_adopt` | **`N`** ← 这个必须是 N，是 `Y` 立刻停手 |
| 5 | `unshare -U true` | `rc=0`（模块会"假成功"，这是预期） |
| 6 | 手机没重启 | ✓ |

> **第 5 条为什么是"假成功"**：App 的能力探测需要它返回成功，否则「允许沙箱」会显示不可用。
> 但它**不建任何命名空间**，也不改任何凭据。详见 [TROUBLESHOOTING ⑨](../TROUBLESHOOTING.md#九如何正确判定-userns)。

## 4. 打开开机自启（可选，建议观察一天后再开）

WebUI：KernelSU → 模块 → Droidspaces → 状态页

| 项 | 建议值 |
|---|---|
| 开机自动加载 | **开** |
| 延迟时长 | 30 秒 |
| bootloop 守卫 | **开** |
| 加载内容 | 全部勾上 |

延迟 30 秒是因为开机瞬间 `/data` 还没挂好，模块读不到文件会失败。

> ⚠️ 容器那边的 `run_at_boot` 要和这边对齐。
> 模块自启 + 容器自启 = 正常；模块不自启 + 容器自启 = **每次开机白失败一次**。
> 见 [TROUBLESHOOTING ⑦](../TROUBLESHOOTING.md#七每次开机容器白失败一次)。

## 5. 装完后的目录长什么样

```
/data/adb/modules/Droidspaces/
├── module.prop            版本信息
├── post-fs-data.sh        加载脚本（PH_ACTION=1 手动跑）
├── service.sh             延迟加载 / 守卫
├── uninstall.sh           卸载时清理
├── userns.sh              属主脚本（status / list / check）
├── ko/                    六个 .ko
├── webroot/index.html     WebUI
├── tools/fake_config.sh   让 App 探测过关
└── no_boot                存在 = 开机不加载
```

## 6. 出问题怎么办

| 症状 | 看这里 |
|---|---|
| 加载失败，说机型/内核不匹配 | 你的内核版本不是 6.12.69，**不要改锁**，换机器 |
| 加载成功但容器起不来 | [TROUBLESHOOTING ③](../TROUBLESHOOTING.md#三容器起不来) |
| 手机重启了 | 立刻确认 `userns_adopt` 是 `N`，看 [①](../TROUBLESHOOTING.md#一整机重启--关机) |
| 模块列表显示"已加载 6/6"但 `/proc/modules` 是空的 | WebUI 那行是 `desc.sh` 写的**文字**，不是实时状态，以 `/proc/modules` 为准 |

---

下一步 → [② 第一个容器](02-container.md)
