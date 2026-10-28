# 第三方源码归属与许可证

本仓库（`droidspaces-lkm-suite`）的自有代码以 **GPL-2.0** 发布，
见根目录 `LICENSE`。

以下第三方组件各自保留原版权与许可证，随包附带的源码快照位于
`droid_lkm/deps/`。

---

## 1. droid_lkm 系列（主模块）

| 组件 | 来源 | 许可证 | 用途 |
| --- | --- | --- | --- |
| `_patchHarmony`（`droid_lkm/src/`） | https://github.com/dere3046/_patchHarmony | GPL-2.0 | PID_NS / IPC_NS / SysV IPC / POSIX mqueue / userns / devtmpfs / 厂商兼容 / inline hook |

`droid_lkm/src/` 目录下的源码来自上游 `_patchHarmony` 项目，
Copyright 保留原作者，本仓库的修改（userns 重写、devtmpfs 改为只注册、
机型/内核守卫配合等）同样以 GPL-2.0 发布。

上游致谢名单：

- [XGH2333](https://github.com/XGH2333) — O3 CPU support
- [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS) — 目标容器运行时
- [KMSDK](https://github.com/Dere3046/KMSDK) — 依赖获取 SDK

## 2. 运行时依赖库（`droid_lkm/deps/`）

| 组件 | 来源 | 版本（deps.lst） | 许可证 | 用途 |
| --- | --- | --- | --- | --- |
| KallRecon | https://github.com/Dere3046/KallRecon | `b258fb6` | GPL-2.0 | 运行期从内核镜像恢复 kallsyms 表，等价内核内部 `kallsyms_lookup_name` |
| Type_info | https://github.com/Dere3046/Type_info | `7182dae` | GPL-2.0 | 运行期重建内核结构体布局（Type-info BTF） |
| KernCall | https://github.com/Dere3046/KernCall | `cdec722` | GPL-2.0 | 系统调用表改写 |
| HooKern | https://github.com/Dere3046/HooKern | `7a02afa` | GPL-2.0 | inline hook / kprobe 引擎 |

各组件的 LICENSE 文件保留在 `droid_lkm/deps/<Name>/LICENSE`。

## 2.1 KMSDK 内置库

| 组件 | 位置 | 版本 | 许可证 | 用途 |
| --- | --- | --- | --- | --- |
| vendorcompat | `droid_lkm/.sdk/builtin/vendorcompat/` | SDK builtin | GPL-2.0-only（源文件 SPDX 头，Copyright (C) 2026 dere3046） | 厂商内核兼容层，被 `droid_lkm_vendor.ko` 引用 |
| KMSDK | https://github.com/Dere3046/KMSDK | `b8a5c64`（见 `droid_lkm/.sdk-version`） | 见上游 | 依赖装配 SDK；`droid_lkm/.sdk/builtin/` 即其内置库快照 |

`droid_lkm/.sdk/builtin/` 里只保留了本项目实际用到的 `vendorcompat`，
其余 SDK 内置库已剔除。

## 3. 内核源码派生代码

| 文件 | 来源 | 许可证 |
| --- | --- | --- |
| `net_macvlan/macvlan_core.c` | Linux 内核 `drivers/net/macvlan.c` | SPDX GPL-2.0-or-later；Copyright (c) 2007 Patrick McHardy；含 Alex Zeffertt / Ben Greear 的历史版权声明 |
| `net_ip6nat/ip6table_nat_core.c` | Linux 内核 `net/netfilter/ip6tables/ip6table_nat.c` | SPDX GPL-2.0-only；Copyright (c) 2011 Patrick McHardy |

两者的原始版权头均**逐字保留**在文件顶部，未删除。

## 4. net_* 目录内的 KallRecon 副本

`net_macvlan/kr/` 与 `net_ip6nat/kr/` 是 KallRecon `lib/` 的副本（逐字节相同），
用于让两个网络模块独立成可单独构建的 Kbuild 目标。
许可证同 KallRecon（GPL-2.0），原始 LICENSE 保留在 `droid_lkm/deps/KallRecon/LICENSE`。

## 5. 本仓库自有部分（GPL-2.0）

- `dsm/post-fs-data.sh` —— 守卫、开关、自解析优先 + 传参回退、bootloop 守卫
- `dsm/service.sh` —— 存活守卫
- `dsm/webroot/index.html` —— WebUI
- `dsm/module.prop`、`dsm/uninstall.sh`、`dsm/tools/action.sh`
- `net_macvlan/macvlan_shim.{c,h}` —— 7 个未导出符号重定向 shim
- `net_ip6nat/ip6t_nat_shim.{c,h}` —— 9 个未导出符号重定向 shim
- `scripts/*` —— 构建与打包脚本
- `README.md` / `BUILD.md` / `VERIFY.md` / `USAGE.md` / `THIRD-PARTY.md`

## 6. 商标与致谢

- "Droidspaces" 是 Droidspaces-OSS 项目的名称，本仓库只是为其提供内核侧支撑模块，
  不声称对该项目有任何所有权或官方关联。
- "OnePlus / 一加 / OPPO" 及其商标归各自权利人所有，本仓库与之无隶属关系。
