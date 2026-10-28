#!/system/bin/sh
# late_start service —— 存活确认
#
# post-fs-data.sh 加载成功后留下 boot_pending；本脚本等 120 秒，
# 若系统还活着就写 .boot_ok 并把 boot_pending 删掉。
# 下次开机若看到 boot_pending 还在、.boot_ok 不在，说明上次是"加载完才崩"
# （insmod 成功但几十秒后 panic），post-fs-data.sh 会据此建 blocked 跳过加载。
#
# 这一段补的是 insmod.inflight 覆盖不到的窗口：那种"能开机、用一会儿才崩"的情况。
#
# 存活窗口可用存活文件改: echo 300 > /data/adb/modules/Droidspaces/alive_sec

MODDIR=${0%/*}

# App 可能在任何时候重新探测内核能力，bind mount 掉的话这里补回来
[ -f "$MODDIR/tools/fake_config.sh" ] && sh "$MODDIR/tools/fake_config.sh" >/dev/null 2>&1

PEND="$MODDIR/boot_pending"

[ -f "$PEND" ] || exit 0

SEC=120
if [ -f "$MODDIR/alive_sec" ]; then
  V=$(tr -dc '0-9' <"$MODDIR/alive_sec" 2>/dev/null)
  [ -n "$V" ] && [ "$V" -gt 0 ] && [ "$V" -le 900 ] && SEC=$V
fi

sleep "$SEC"

# 已经被守卫拦下或用户已恢复, 不重复标记
[ -f "$MODDIR/blocked" ] && { rm -f "$PEND" 2>/dev/null; exit 0; }

: >"$MODDIR/.boot_ok" 2>/dev/null
rm -f "$PEND" 2>/dev/null
exit 0
