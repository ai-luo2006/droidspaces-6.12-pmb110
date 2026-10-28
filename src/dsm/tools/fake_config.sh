#!/system/bin/sh
# /proc/config.gz 伪装
#
# Droidspaces App 判断「允许沙箱 / 硬件访问」是否可用，读的是内核配置里的
#   CONFIG_USER_NS
#   CONFIG_DEVTMPFS
# 本机内核两项都是 "not set"，所以它直接判死，连 unshare 都不调用
# （dmesg 里一条 unshare/clone 记录都没有），并且把 allow_sandboxing 写回 0。
#
# 模块在运行时能补的只有 devtmpfs（注册一个同名文件系统，让 /proc/filesystems
# 里出现它）。userns 补不了 —— 内核没编译 CONFIG_USER_NS，模块只能在 syscall
# 边界做 id 翻译，拿不到真正的命名空间。
#
# 这里把两行都改成 y，纯粹是让 App 的探测过关、不再把 allow_sandboxing 写回 0。
# 它伪装的是「能力声明」，不是「容器真的进了 user namespace」：
# App 读到的 CONFIG_USER_NS=y 是假的，容器进程的 uid_map 仍然是
# 0 0 4294967295。这一点在 README.txt「真 userns —— 本机做不到」里有说明。
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
