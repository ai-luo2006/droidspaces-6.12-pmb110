#!/system/bin/sh
# /proc/config.gz 伪装
#
# Droidspaces App 判断「允许沙箱 / 硬件访问」是否可用，读的是内核配置里的
#   CONFIG_USER_NS
#   CONFIG_DEVTMPFS
# 本机内核两项都是 "not set"，所以它直接判死，连 unshare 都不调用
# （dmesg 里一条 unshare/clone 记录都没有），并且把 allow_sandboxing 写回 0。
#
# 模块已经在运行时把这两项能力补齐了（userns 走 hooks，devtmpfs 走注册的
# 文件系统），所以这里把配置文本里这两行改成 y，再 bind mount 覆盖回
# /proc/config.gz。App 读到的判定就和真实能力一致了。
#
# 原文件仍在内核里，umount /proc/config.gz 立刻恢复；卸载模块时 uninstall.sh
# 也会做这件事。

MODDIR=${0%/*}/..
[ -d "$MODDIR" ] || MODDIR=/data/adb/modules/Droidspaces

SRC=/proc/config.gz
DST="$MODDIR/etc/config.gz"

[ -f "$SRC" ] || exit 0
mkdir -p "$MODDIR/etc" 2>/dev/null

# 已经改过就不用再生成，但挂载掉了要补回来
if [ -f "$DST" ]; then
	# 挂载点还在（bind 生效）就直接退出，避免每次重复 umount/mount
	if mount | grep -q " $SRC "; then
		exit 0
	fi
fi

TMP="$DST.new"
zcat "$SRC" 2>/dev/null |
	sed -e 's/^# CONFIG_USER_NS is not set$/CONFIG_USER_NS=y/' \
	    -e 's/^# CONFIG_DEVTMPFS is not set$/CONFIG_DEVTMPFS=y/' |
	gzip -c >"$TMP" 2>/dev/null
if [ ! -s "$TMP" ]; then
	rm -f "$TMP"
	exit 0
fi
mv -f "$TMP" "$DST"

# 沿用 /proc/config.gz 的 SELinux 标签，否则普通 App 读不到
CTX=$(ls -Zd "$SRC" 2>/dev/null | awk '{print $1}')
[ -n "$CTX" ] && chcon "$CTX" "$DST" 2>/dev/null

umount "$SRC" 2>/dev/null
if mount --bind "$DST" "$SRC" 2>/dev/null; then
	echo "config.gz: CONFIG_USER_NS / CONFIG_DEVTMPFS 已标记为 y"
else
	echo "config.gz: bind mount 失败（SELinux 或只读 /proc）"
fi
exit 0
