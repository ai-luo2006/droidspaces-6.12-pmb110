#!/system/bin/sh
#
# 容器内进程数压测：证明 --pids-limit 是真拦截，不是显示层的壳子。
#
# 原理：把容器 cgroup 的上限设成「当前进程数 + 10」，然后让容器 fork 60 个
#       长期存活的进程。
#         - 限制不生效 -> current 会一路涨到 ~当前+60，被拒次数 ≈ 0
#         - 限制生效   -> current 卡在 ~当前+10，  被拒次数 ≈ 50
# 判定只依赖宿主机读到的 current 和 pids.stats，不依赖容器内的回显
# （容器内 shell 想 fork 也会被拒，所以它自己往往说不出话）。
#
# 用法： su -c 'sh /storage/emulated/0/1/2/stress-pids.sh'

CG=/sys/fs/cgroup/droidspaces/root
DS=/data/local/Droidspaces/bin/droidspaces

if [ ! -d "$CG" ]; then
	echo "[!] 找不到 $CG，容器还没启动？先启动容器再跑本脚本。"
	exit 1
fi

field() {
	cat "$CG/pids.stats" 2>/dev/null | tr ' ' '\n' | grep "^$1=" | cut -d= -f2
}

OLD=$(cat "$CG/pids.max" 2>/dev/null)
CUR=$(cat "$CG/pids.current" 2>/dev/null)
R0=$(field refused)

if [ -z "$OLD" ] || [ -z "$CUR" ]; then
	echo "[!] 读不到 pids.max / pids.current，模块没加载或容器目录没挂上。"
	exit 1
fi

NEW=$((CUR + 10))
echo "=============================================="
echo " 压测前：上限=$OLD  容器内进程数=$CUR"
echo " 临时把上限设为 $NEW（只允许再增加 10 个）"
echo "=============================================="
echo "$NEW" > "$CG/pids.max"

# 容器内 fork 60 个存活 25 秒的进程。timeout 兜底，避免 fork 被拒时卡死。
timeout 25 "$DS" --name=root exec -- /bin/sh -c \
	'i=0; while [ $i -lt 60 ]; do (sleep 25 &) 2>/dev/null; i=$((i+1)); done; sleep 3; ls /proc 2>/dev/null | grep -c ^[0-9]' \
	2>/dev/null
echo "（上面若无数字，是容器内 shell 自己 fork 被拒、说不出话 —— 这本身就是限制生效的表现）"

sleep 1
AFTER=$(cat "$CG/pids.current" 2>/dev/null)
R1=$(field refused)
DENIED=$((R1 - R0))

echo
echo "=============================================="
echo " 压测后：容器内进程数 = $AFTER   （上限 $NEW）"
echo " 期间被拒绝的 fork 次数 = $DENIED"
echo "=============================================="
echo "--- 完整 stats ---"
cat "$CG/pids.stats"

if [ "$AFTER" -le $((NEW + 5)) ] && [ "$DENIED" -gt 0 ]; then
	echo
	echo "[OK] 限制真实生效：进程数被卡在 $NEW 附近，且 $DENIED 次 fork 被 -EAGAIN 拒绝。"
else
	echo
	echo "[!] 疑似未生效：进程数涨到 $AFTER，被拒 $DENIED 次。把上面 stats 发我。"
fi

echo "$OLD" > "$CG/pids.max"
echo
echo "已恢复原上限 = $(cat "$CG/pids.max")"
