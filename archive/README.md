# archive — 历史版本

⚠️ **这个目录里的包不要用来安装。** 保留只是为了追溯问题、对比 md5、复现当时的故障。

| 文件 | 状态 | 一句话 |
|---|---|---|
| `Droidspaces-v1.0.18-6.12.zip` | 🔴 **禁止使用** | `kernel_clone` 钩子，**两次整机重启** |
| `Droidspaces-v1.0.17-6.12.zip` | 🟠 半成品 | 建成 mnt ns，但子进程凭据未切换 |
| `Droidspaces-v1.0.16-6.12.zip` | ⚪ 无效 | 挂错钩子（`fork`），从未触发 |
| `Droidspaces-v1.0.15-6.12.zip` | 🟡 可用 | 卸载引用检查；首次引入 `fake_config.sh` |
| `Droidspaces-v1.0.14-6.12.zip` | 🟡 可用 | 非 root `unshare` 假成功 |
| `Droidspaces-v1.0.13-6.12.zip` | 🟡 可用 | `setgroups` / `uid_map` 写入放行 |
| `Droidspaces-v1.0.12-6.12.zip` | 🟡 可用 | relabel 脚本改为 toybox 兼容 |
| `Droidspaces-v1.0.11-6.12.zip` | ⚪ 旧 | relabel 用 `find -exec {} +`，Android 上静默无效 |
| `Droidspaces-v1.0.10-6.12.zip` | ⚪ 旧 | 本项目基线 |

## md5

```
4f44e3e614ddbbc369ba56c1e4a65ef9  Droidspaces-v1.0.18-6.12.zip   🔴
90c04d42addf06158ff40735b8c36df5  Droidspaces-v1.0.17-6.12.zip
a7a17bdf7c730f3693d2d4768175a93a  Droidspaces-v1.0.16-6.12.zip
3b89304978d5dc3aaae2592a310ccca8  Droidspaces-v1.0.15-6.12.zip
7c166d7fc4c0d3607e1cc06b93654824  Droidspaces-v1.0.14-6.12.zip
2da14ec069a9b16b085bc2741d390ad6  Droidspaces-v1.0.13-6.12.zip
5e96c623c5b6bc573050740b5e42e3c9  Droidspaces-v1.0.12-6.12.zip
d686eb64c3514d1ec91c24e5e2e0956c  Droidspaces-v1.0.11-6.12.zip
2499be3bfbbc752e8dc152a25bdfe360  Droidspaces-v1.0.10-6.12.zip
```

## 🔴 关于 v1.0.18

这一版把「容器进程凭据切换」挂在 `kernel_clone` 上，而 `kernel_clone` 是**每次 fork 都走**的全局热路径。
它把 cred 的 `user_ns` 换成模块 `kmalloc` 出来的伪造结构，`cap_capable()` 遍历
`cred->user_ns->parent` 时读到野指针。表现是手机直接关机，`dmesg` 什么都没有，
`pstore` 里只有联发科 `CLKCHK` + `mrdump`。

**如果你手上还留着这一版，删掉。** 当前版本是 [`../release/v1.0.20/`](../release/v1.0.20/)。

## 未收录的版本

v1.0.0 ~ v1.0.9，以及 2026-10-27 的早期调试包（`droidspaces_6.12_v3` ~ `v36`、`droidspaces_6.12_full` 等）。

那是模块结构定型之前的临时构建，目录布局与现在不同，放进来只会造成混淆。
各版本改了什么见 [`../CHANGELOG.md`](../CHANGELOG.md)。
