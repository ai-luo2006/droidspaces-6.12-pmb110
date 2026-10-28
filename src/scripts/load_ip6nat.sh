#!/system/bin/sh
# 单独加载 ip6table_nat.ko（v30 自解析版）
# 模块在 init 里自己查 9 个未导出符号的地址，所以只要 insmod。
# 用法: sh load_ip6nat.sh [/path/to/ip6table_nat.ko]   默认 /data/local/tmp/ip6table_nat.ko

KO=${1:-/data/local/tmp/ip6table_nat.ko}
[ -f "$KO" ] || { echo "找不到 $KO"; exit 1; }

if grep -q '^ip6table_nat ' /proc/modules 2>/dev/null; then
	echo "ip6table_nat 已加载，先: rmmod ip6table_nat"
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
	for s in ip6t_alloc_initial_table ip6t_register_table ip6t_unregister_table_exit ip6t_do_table xt_find_table xt_register_template xt_unregister_template nf_nat_ipv6_register_fn nf_nat_ipv6_unregister_fn; do
		v=$(grep " $s\$" /proc/kallsyms 2>/dev/null | head -1 | awk '{print $1}')
		case "$v" in
			""|0000000000000000|00000000*) echo "✗ 拿不到 $s 的地址"; exit 1 ;;
		esac
		A="$A addr_$s=0x$v"
	done
	insmod "$KO" $A && echo "✓ 回退加载成功" || { echo "✗ 回退也失败"; dmesg | grep -iE "ip6table_nat|Unknown symbol" | tail -10; exit 1; }
fi

echo "--- dmesg ---"
dmesg | grep -iE "ip6t-nat-shim|ip6table_nat" | tail -6
echo "--- 验证: 列出 ip6 nat 表 ---"
ip6tables -t nat -L -n 2>&1 | head -6
