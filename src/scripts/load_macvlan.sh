#!/system/bin/sh
# 单独加载 macvlan.ko（v30 自解析版）
# 模块自己在 init 里从内核 kallsyms 表查 7 个未导出符号的地址，
# 所以这里只要 insmod，不需要 addr_xxx=，也不需要改 kptr_restrict。
# 用法: sh load_macvlan.sh [/path/to/macvlan.ko]   默认 /data/local/tmp/macvlan.ko
KO=${1:-/data/local/tmp/macvlan.ko}
[ -f "$KO" ] || { echo "找不到 $KO"; exit 1; }

if grep -q '^macvlan ' /proc/modules 2>/dev/null; then
	echo "macvlan 已加载，先卸载: rmmod macvlan"
	exit 0
fi

echo "加载 $KO"
if insmod "$KO"; then
	echo "✓ insmod 成功（自解析）"
else
	rc=$?
	echo "✗ insmod 失败 rc=$rc"
	echo "--- 回退: 脚本读 kallsyms 传地址 ---"
	echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null
	A=""
	for s in dev_forward_skb __dev_forward_skb dev_mc_add_excl ip_check_defrag ndo_dflt_fdb_dump eth_header_cache eth_header_cache_update; do
		v=$(grep " $s\$" /proc/kallsyms 2>/dev/null | head -1 | awk '{print $1}')
		case "$v" in
			""|0000000000000000|00000000*) echo "✗ 拿不到 $s 的地址"; exit 1 ;;
		esac
		A="$A addr_$s=0x$v"
	done
	insmod "$KO" $A && echo "✓ 回退加载成功" || { echo "✗ 回退也失败"; dmesg | grep -iE "macvlan|Unknown symbol" | tail -8; exit 1; }
fi

echo "--- dmesg ---"
dmesg | grep -iE "macvlan-shim|macvlan:" | tail -6
echo "--- 验证 ---"
echo "ip link add mvtest link wlan0 type macvlan mode bridge && ip link del mvtest"
