# release — 发布包

| 目录 | 标记 | 说明 |
|---|---|---|
| `v1.0.20/` | 🟢 **CURRENT** | 当前版本，装这个 |
| `v1.0.19/` | 🟡 ROLLBACK | 上一版，可回退 |

---

## 🟢 v1.0.20 — 当前版本

```
包名    Droidspaces-v1.0.20-6.12.zip
大小    7.2 MB
md5     fef08ae8ca10009632aee397c36d6e3c
适用    PMB110 / 内核 6.12.69-* / KernelSU
```

### 文件清单

| 文件 | 大小 | md5 | 用途 |
|---|---|---|---|
| `Droidspaces-v1.0.20-6.12.zip` | 7.2 MB | `fef08ae8ca10009632aee397c36d6e3c` | 完整 KernelSU 模块包 |
| `droid_lkm_misc.ko` | 4.5 MB | `15fc08cbd67c06c3aa079bedd75b51bc` | 单独的内核模块（快速替换） |
| `webui-index.html` | 75 KB | `75f679574e0347b46eead02789b405a7` | 单独换 WebUI 状态页 |
| `userns.sh` | 12 KB | `4c49905b36aeeac95460730762eb42f1` | 属主脚本（relabel 已加护栏） |
| `fake_config.sh` | 1.8 KB | `2c5f89b7baed20a0544bd22064f43ade` | 伪装 `/proc/config.gz`（⚠️ 只是骗 App） |
| `Droidspaces-6.12-src-v1.0.20.zip` | 710 KB | `8a60aef52cd7192ba71c6a6b8a969ae1` | 源码快照（与 `../src/` 内容一致） |

### 安装

**A. 完整包**（推荐）

KernelSU → 模块 → 安装 → 选 zip → 重启

**B. 只换 ko**（不用重启）

```sh
su -c 'droidspaces --config=/data/local/Droidspaces/Containers/root/container.config stop
       echo 1 > /sys/module/droid_lkm_misc/parameters/release
       rmmod droid_lkm_misc
       cp /sdcard/Download/droid_lkm_misc.ko /data/adb/modules/Droidspaces/ko/
       insmod /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko real_userns=1 userns_any=1
       md5sum /data/adb/modules/Droidspaces/ko/droid_lkm_misc.ko'
```

**C. 只换页面**（最轻，容器不受影响）

```sh
su -c 'cp /sdcard/Download/webui-index.html /data/adb/modules/Droidspaces/webroot/index.html
       md5sum /data/adb/modules/Droidspaces/webroot/index.html'
```

### 验证

```sh
su -c 'unshare -U true; echo "rc=$?"'        # 期望 rc=0
su -c 'dmesg | grep "flag dropped" | tail -2' # 期望有 flag dropped 记录
```

然后开容器、进容器：

```sh
id              # uid=0(root)
ls -ln /        # 属主全 0 0
su -c id        # uid=0(root)，不报 cannot set groups
```

### v1.0.20 改了什么

只改外围，**ko 与 v1.0.19 完全相同**：

- WebUI 按容器真实 `uid_map` 判定 userns，不再拿模块自报的 `real=1` 当"已生效"
- 探针文案 `18/18 · 真映射` → `18/18 · 钩子已挂`
- **禁用「转换容器属主」按钮**（点一次 = 52 万文件 + 132 秒 restore + 容器坏掉）
- `userns.sh relabel` 加硬护栏
- 文档全面纠正过时断言

---

## 🟡 v1.0.19 — 可回退

```
包名    Droidspaces-v1.0.19-6.12.zip
md5     63c19b0fbdb25e56d9c62028e08170f0
```

内核模块与 v1.0.20 **完全相同**（`15fc08cbd67c06c3aa079bedd75b51bc`）。
只差 WebUI：v1.0.19 的页面会把「真 userns」显示成"已生效"，且「转换容器属主」可点。

**只在 v1.0.20 页面出问题时才回退**，回退后注意别点那个按钮。

---

## 通用注意

| 事项 | 说明 |
|---|---|
| 机型校验 | 模块自带 `PMB110 + 6.12.69-*` 锁，别的机型不会加载 |
| 首次安装 | 默认关闭开机自启（`no_boot`），需手动点一次「加载」验证 |
| 卸载前 | **先停容器**，否则可能 panic（见 `docs/TROUBLESHOOTING.md` ⑥） |
| 不要 relabel | 本机无 userns，改了就是弄坏容器 |

校验全部文件（在**仓库根目录**执行，`checksums.txt` 里的路径是相对根的）：

```sh
md5sum -c checksums.txt --ignore-missing
```
