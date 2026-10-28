#!/system/bin/sh
# KernelSU「执行」按钮 / 手动调用
#
#   sh action.sh load     加载 droid_lkm_pids.ko（跳过机型锁，不写守卫标记）
#   sh action.sh unload   卸载
#   sh action.sh status   查看状态 + pids.stats
#
# 手动加载不经过 no_boot / 机型锁，方便调试。

MODDIR=${0%/*}
KO="$MODDIR/ko/droid_lkm_pids.ko"
LOG="$MODDIR/load.log"

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

do_load() {
	if [ -f /proc/modules ] && grep -q '^droid_lkm_pids ' /proc/modules; then
		echo "already loaded"
		return 0
	fi
	if [ ! -f "$KO" ]; then
		echo "ko missing: $KO"
		return 1
	fi
	insmod "$KO" 2>&1 | tee -a "$LOG"
	rc=${PIPESTATUS[0]:-$?}
	[ "$rc" = 0 ] && log "manual insmod ok" || log "manual insmod rc=$rc"
	echo "--- dmesg ---"
	dmesg | grep droid_lkm_pids | tail -12
	return $rc
}

do_unload() {
	rmmod droid_lkm_pids 2>&1 | tee -a "$LOG"
	log "manual rmmod"
	dmesg | grep droid_lkm_pids | tail -3
}

do_status() {
	if grep -q '^droid_lkm_pids ' /proc/modules 2>/dev/null; then
		echo "droid_lkm_pids: LOADED"
	else
		echo "droid_lkm_pids: not loaded"
	fi
	echo "--- /proc/cgroups (pids) ---"
	grep -w pids /proc/cgroups 2>/dev/null || echo "(no pids line)"
	echo "--- pids.stats of running containers ---"
	for d in /sys/fs/cgroup/*/; do
		[ -f "${d}pids.stats" ] || continue
		printf '%s ' "$d"
		cat "${d}pids.stats"
	done
	echo "--- last log ---"
	tail -20 "$LOG" 2>/dev/null
}

case "$1" in
	load)   do_load ;;
	unload) do_unload ;;
	*)      do_status ;;
esac
exit 0
