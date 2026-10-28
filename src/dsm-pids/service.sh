#!/system/bin/sh
# 存活守卫：insmod 之后系统若撑过 alive_sec 秒，写 .boot_ok。
# 下次开机若 boot_pending 还在而 .boot_ok 不在，说明上次加载后崩了。

MODDIR=${0%/*}
LOG="$MODDIR/load.log"

[ -f "$MODDIR/boot_pending" ] || exit 0

SEC=120
[ -f "$MODDIR/alive_sec" ] && read -r SEC < "$MODDIR/alive_sec"
case "$SEC" in
	''|*[!0-9]*) SEC=120 ;;
esac
[ "$SEC" -gt 900 ] && SEC=900

sleep "$SEC"

if [ -d /proc/1 ] && [ -d /sys/fs/cgroup ]; then
	: > "$MODDIR/.boot_ok"
	rm -f "$MODDIR/boot_pending"
	echo "$(date '+%F %T') alive: boot_ok" >> "$LOG"
else
	echo "$(date '+%F %T') not alive after ${SEC}s" >> "$LOG"
fi
exit 0
