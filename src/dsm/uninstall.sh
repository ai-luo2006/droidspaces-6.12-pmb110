#!/system/bin/sh
# 卸载时尽量把模块从内存移除（内核不允许卸载被占用的模块，这里只是尽力）
# 顺序不能反：pids 可单独卸载，misc 持有 devtmpfs 内部 mount 要先放掉，lkm 最后
MODDIR=${0%/*}

# 真 userns 会把容器 rootfs 里 root 的文件改到 base 名下。模块卸载后没有 uid 翻译，
# 原生 Droidspaces 会读不动这些文件，所以先提醒改回来
for r in /data/local/Droidspaces/Containers/*/rootfs/.droid-userns-owner; do
	[ -f "$r" ] || continue
	echo "[!] 有容器做过真 userns 属主转换，卸载后 Droidspaces 会读不动它的 rootfs。"
	echo "    请先执行: sh $MODDIR/userns.sh restore all"
	break
done

# 撤掉 /proc/config.gz 的伪装，把真实内核配置还回去
umount /proc/config.gz 2>/dev/null

for m in ip6table_nat macvlan droid_lkm_pids droid_lkm_misc droid_lkm_vendor droid_lkm; do
	rmmod "$m" 2>/dev/null
done

# 让模块列表里的描述回到"没加载"的原文案
[ -f "$MODDIR/desc.sh" ] && sh "$MODDIR/desc.sh" >/dev/null 2>&1
exit 0
