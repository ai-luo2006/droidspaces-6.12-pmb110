#!/system/bin/sh
#
# 进程数限制的硬验证（不依赖容器、不依赖容器内回显）
#
# 思路：在宿主机 cgroup 树上新建一个测试 cgroup，设 pids.max=5，把一个 shell
#       移进去，让它 fork 20 个长期存活的进程。然后由【外部不受限的 shell】
#       去数这个 cgroup 里到底有几个进程。
#
# 关键点：受控 shell 想 fork 就会被拒（连"报数"这个动作都做不了），所以判定
#         必须由站在外面的 shell 来做 —— 读 cgroup.procs 的行数。
#
#   - 限制不生效 -> cgroup.procs 应有 ~21 行（1 个 shell + 20 个 sleep）
#   - 限制生效   -> cgroup.procs 只有 ~6 行，且 pids.stats 里 refused 明显增加
#
# 用法： su -c 'sh /storage/emulated/0/1/2/stress-pids2.sh'

CG=/sys/fs/cgroup/stress_pids_b
REF=/sys/fs/cgroup/droidspaces/root/pids.stats

field() {
	cat "$REF" 2>/dev/null | tr ' ' '\n' | grep "^$1=" | cut -d= -f2
}

# 清理上一轮残留
rmdir "$CG" 2>/dev/null

# memory 必须挂在根上：子 cgroup 会继承 memory css，pids.max 才会出现。
# 注意只能写根这一层 —— 给子 cgroup 自己写 +memory 会触发 v2 的
# no-internal-process 规则，之后就放不进进程了。
echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null

mkdir -p "$CG" 2>/dev/null
if [ ! -d "$CG" ]; then
	echo "[!] 无法创建 $CG"
	exit 1
fi

if [ ! -f "$CG/pids.max" ]; then
	echo "[!] $CG/pids.max 不存在 —— 根 cgroup 没有启用 memory，模块没加载？"
	cat /sys/fs/cgroup/cgroup.subtree_control
	rmdir "$CG" 2>/dev/null
	exit 1
fi

LIMIT=5
echo "$LIMIT" > "$CG/pids.max"

R0=$(field refused)

echo "=============================================="
echo " 测试 cgroup: $CG"
echo " 上限 pids.max = $LIMIT"
echo " 让里面的 shell 尝试 fork 20 个长期存活进程"
echo "=============================================="

# 这个 shell 被放进受控 cgroup，它的 fork 会被拦。timeout 兜底防卡死。
timeout 20 /system/bin/sh -c \
	"echo \$\$ > $CG/cgroup.procs; i=0; while [ \$i -lt 20 ]; do (sleep 15 &) 2>/dev/null; i=\$((i+1)); done; sleep 3" \
	2>/dev/null

sleep 1

# 由外面这个不受限的 shell 来数（它不在受控 cgroup 里，能自由 fork）
CNT=$(cat "$CG/cgroup.procs" 2>/dev/null | grep -c .)
R1=$(field refused)
DENIED=$((R1 - R0))

echo
echo "=============================================="
echo " 结果：$CG 里实际存在的进程数 = $CNT"
echo "       期间被拒绝的 fork 次数  = $DENIED"
echo "=============================================="
echo "--- 参考：容器 cgroup 的完整 stats ---"
cat "$REF"

if [ "$CNT" -le $((LIMIT + 2)) ] && [ "$DENIED" -gt 0 ]; then
	echo
	echo "[OK] 限制真实生效：上限 $LIMIT，实际只进来 $CNT 个，"
	echo "     $DENIED 次 fork 被 -EAGAIN 拒绝。这不是显示层的壳子。"
else
	echo
	echo "[!] 疑似未生效：进来 $CNT 个（上限 $LIMIT），被拒 $DENIED 次。"
	echo "    把上面整段输出发我。"
fi

echo
echo "--- 清理 ---"
pkill -f "sleep 15" 2>/dev/null
sleep 1
rmdir "$CG" 2>/dev/null && echo "已删除 $CG" || echo "$CG 删除失败（可能还有进程残留），可稍后 rmdir"
