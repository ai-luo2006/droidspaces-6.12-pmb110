#!/system/bin/sh
# Droidspaces 方案 B 真机验证脚本
#
# 用法（在 Termux，已取得 root）：
#   1. 把 droid_lkm_pids.ko 放到本目录
#   2. sh verify-pids-b.sh
#
# 它做三件事：
#   A. 加载模块，确认打印 "pids limit enforcement active"
#   B. 创建一个测试 cgroup，确认 pids.max 文件出现且可读写（B1）
#   C. 把当前 shell 移入该 cgroup，设一个很小的上限，fork 一大堆进程，
#      确认真正被拒（超出的 fork 拿不到 - 文件不会被创建）—— 证明限制真实生效（B2）
#
# 全程只产生若干短命的 touch 进程，不会 fork-bomb，也不会刷内核。

set -u

# 切到脚本所在目录，这样从任意目录调用都能找到同目录的 ko
cd "$(dirname "$0")" 2>/dev/null || true

KO="./droid_lkm_pids.ko"

if [ ! -f "$KO" ]; then
	echo "[!] 找不到 $KO，请先把 ko 放到当前目录"
	exit 1
fi

echo "==> A. 加载模块"
insmod "$KO" 2>/dev/null
if ! lsmod 2>/dev/null | grep -q droid_lkm_pids; then
	echo "[!] 加载失败，看 dmesg："
	dmesg | tail -n 20
	exit 1
fi

echo "    dmesg 中应有："
dmesg | grep -i "pids limit enforcement" | tail -n 3
echo "    被跳过的钩子（该内核不可 hook 时会显示 trampoline carries a trap）："
dmesg | grep -i "trampoline carries a trap" | tail -n 5
echo "    （上面若无 trap 行，说明钩子全部装上了）"

echo
echo "==> B. 结构验证：pids.max 是否出现且可读写"
CG=/sys/fs/cgroup/test_pids_b
rmdir "$CG" 2>/dev/null
mkdir -p "$CG" 2>/dev/null || { echo "[!] 无法创建测试 cgroup（需要 root 与 cgroup v2 可写）"; exit 1; }

# 只在根上启用 memory：子 cgroup 会因此继承 memory css，pids.max 就会出现。
# 千万不要再给测试 cgroup 自己写 subtree_control —— 一旦它启用了控制器，
# cgroup v2 的 no-internal-process 规则就不允许把进程放进去了，cgroup.procs
# 迁移会静默失败，后面的 fork 全部发生在原来的 cgroup 里，测出来是假阴性。
echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null

if [ ! -e "$CG/pids.max" ]; then
	echo "[!] pids.max 未出现。可能原因：memory 控制器未挂到该 cgroup，"
	echo "    或 cgroup_add_dfl_cftypes 未成功（dmesg 查看）。"
	rmdir "$CG" 2>/dev/null
	exit 1
fi
echo "    [OK] $CG/pids.max 存在"

echo 5 > "$CG/pids.max" 2>/dev/null || { echo "[!] 写入 pids.max 失败"; rmdir "$CG" 2>/dev/null; exit 1; }
GOT=$(cat "$CG/pids.max" 2>/dev/null)
echo "    写入 5，读回 = $GOT"
[ "$GOT" = "5" ] && echo "    [OK] pids.max 可读写" || echo "    [~] 读回=$GOT（期望 5）"

echo
echo "==> C. 功能验证：限制是否真实拦截 fork"
# 把当前 shell 移入测试 cgroup（迁移本身不计数，容器内 fork 才计数）
echo $$ > "$CG/cgroup.procs" 2>/dev/null

# 迁移失败会让整个 C 步失去意义，所以必须确认 shell 真的在里面
case "$(cat /proc/self/cgroup 2>/dev/null)" in
	*test_pids_b*) echo "    [OK] shell 已移入测试 cgroup" ;;
	*) echo "    [!] shell 未移入测试 cgroup（/proc/self/cgroup=$(cat /proc/self/cgroup 2>/dev/null)），C 步结果无效" ;;
esac

LIMIT=5
echo "    上限设为 $LIMIT，当前 shell 已在该 cgroup 中，尝试 fork 20 个进程"
TMP=/data/local/tmp
rm -f "$TMP"/pids_b_*
# 每个子进程必须活够久：一个瞬间退出的进程会立刻退账，计数器永远累积不到
# 上限，那样测出来的 "没被拦住" 是假阴性。sleep 让它们同时存在，撞上上限。
#
# 超限后 shell 的 & 会打印 "can't fork - try again" 并反复重试 —— 那正是
# 内核返回 -EAGAIN 的表现，也就是限制生效的证据。但脚本不能因此卡死，所以
# 整段用 timeout 兜住。
timeout 12 sh -c '
	TMP="$1"; I=1
	while [ $I -le 20 ]; do
		( echo x > "$TMP/pids_b_$I"; sleep 8 ) &
		I=$((I+1))
	done
	sleep 3
' sh "$TMP" 2>/dev/null
# 统计真正跑起来的进程数（每个成功 fork 的会创建一个文件）
CNT=0
I=1
while [ $I -le 20 ]; do
	[ -f "$TMP/pids_b_$I" ] && CNT=$((CNT+1))
	I=$((I+1))
done
rm -f "$TMP"/pids_b_*

echo "    实际启动进程数 = $CNT（应 <= $LIMIT）"
echo "    pids.current = $(cat "$CG/pids.current" 2>/dev/null)"
echo "    pids.stats  = $(cat "$CG/pids.stats" 2>/dev/null)"
echo "    （charged 应随 fork 一起涨，refused>0 表示超限的 fork 真被拦下）"
if [ "$CNT" -le "$LIMIT" ]; then
	echo "    [OK] 限制真实生效：超出上限的 fork 被拒绝"
else
	echo "    [!] 超出上限仍在运行，限制未生效（检查 dmesg / 钩子是否装上）"
fi

echo
echo "==> 清理"
echo 0 > "$CG/pids.max" 2>/dev/null
# 把 shell 移回根 cgroup 再删除测试 cgroup
echo $$ > /sys/fs/cgroup/cgroup.procs 2>/dev/null
rmdir "$CG" 2>/dev/null
echo "    完成。可用 'rmmod droid_lkm_pids' 卸载。"
