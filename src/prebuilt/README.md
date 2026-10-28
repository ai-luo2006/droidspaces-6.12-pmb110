# 预编译产物

6 个 `.ko` 已随包提供在 [`../dsm/ko/`](../dsm/ko/)（那里是 KernelSU 模块实际使用的位置）。

校验和见 [`MD5SUMS.txt`](MD5SUMS.txt)：

```
95866f6fe63db29a082ebc843ed69988  droid_lkm.ko
7b761775e8f2bb17b71efd77fff1f27a  droid_lkm_vendor.ko
33fc92642667a70e13a43cb3b6c8d503  droid_lkm_misc.ko
caa8b9476d55a447f47738bcec371dc4  droid_lkm_pids.ko
5694f4456b664d6ff8c1004a527eb165  macvlan.ko
8b2b364d0a5b0fc7ba2d658700eb4340  ip6table_nat.ko
```

这四个主模块由本仓库源码用 DDK (`android16-6.12`) 重新编译后，
md5 与上表**逐字节一致**，构建可复现。

`droid_lkm_pids.ko` 含 pids 显示层与功能层，可单独 `rmmod` / `insmod`；
功能层的自检方式见
[`../docs/PIDS-ENFORCEMENT.md`](../../docs/PIDS-ENFORCEMENT.md#6-pidsstats自检计数器)
（`pids.stats` 里 `refused > 0` 即生效）。

网络模块（macvlan / ip6table_nat）重新编译后 md5 会不同，
原因是 `.text` 里嵌入了编译时的源文件绝对路径（`__FILE__`），
导出符号表则完全一致。功能等价。

> ⚠ 这批 ko **只在 PMB110 / 6.12.69 上真机验证过**。
> 别的机型/内核请看 [`../docs/BUILD-FOR-OTHERS.md`](../../docs/BUILD-FOR-OTHERS.md) 重新编译。
