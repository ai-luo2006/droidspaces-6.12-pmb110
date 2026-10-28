#!/system/bin/sh
# Droidspaces 真 userns 工具：把容器 rootfs 的属主对到宿主 uid
#
# 为什么需要它
#   模块把容器里的 uid 0 变成宿主上的 userns_base（默认 200000）。容器 rootfs 里
#   的文件属主原本是 0，容器里一读就是"别人的文件"，整棵树都打不开。所以要把
#   rootfs 里 root 拥有的东西改到 base 名下——容器里再用 uid 翻译读回 0，两边对上。
#
# 用法
#   userns.sh status              看模块状态与 base
#   userns.sh list                列容器 + 属主状态
#   userns.sh check  <名字|all>   只统计还差多少，不改
#   userns.sh relabel <名字|all> [--force]
#
# 只需要做一次。容器 rootfs 被重建（换发行版/重装）之后再做一次。

DIR=/data/local/Droidspaces/Containers
ST=/proc/droid_lkm_misc/status
MARK=.droid-userns-owner

ACT=${1:-list}
ONLY=${2:-}
FORCE=0
[ "$3" = "--force" ] && FORCE=1

base_of() {
	b=$(sed -n 's/.*base=\([0-9][0-9]*\).*/\1/p' "$ST" 2>/dev/null | head -1)
	if [ -n "$b" ]; then echo "$b"; else echo 200000; fi
}

real_on() {
	grep -q 'real=1' "$ST" 2>/dev/null
}

# 有没有任何进程真的在 user namespace 里。
# 判据是 /proc/PID/uid_map 不是 init_user_ns 的全量恒等映射 —— 只有真进了 userns 的
# 进程才会写出 "0 <base> 65536" 这种范围。内核没编 CONFIG_USER_NS 时，这个文件里
# 永远是 "0 0 4294967295"。全用 shell 内建 read，不 fork，几百进程也很快。
ns_live() {
	for f in /proc/[0-9]*/uid_map; do
		[ -r "$f" ] || continue
		u1=; u2=; u3=
		read -r u1 u2 u3 2>/dev/null <"$f" || continue
		[ -n "$u1" ] || continue
		[ "$u1" = 0 ] && [ "$u2" = 0 ] && [ "$u3" = 4294967295 ] && continue
		return 0
	done
	return 1
}

os_name() {
	sed -n 's/^PRETTY_NAME=//p' "$1/rootfs/etc/os-release" 2>/dev/null | tr -d '"' | head -1
}

# 容器是否正挂在 /proc/mounts 里（运行中不能改属主）
is_running() {
	grep -q " $1/rootfs " /proc/mounts 2>/dev/null
}

# 目标容器列表：只认有 rootfs/etc 的目录
targets() {
	for d in "$DIR"/*/; do
		c=${d%/}
		[ -d "$c/rootfs/etc" ] || continue
		if [ -n "$ONLY" ] && [ "$ONLY" != all ]; then
			[ "$(basename "$c")" = "$ONLY" ] || continue
		fi
		echo "$c"
	done
}

do_status() {
	echo "模块状态:"
	grep -i 'userns' "$ST" 2>/dev/null || echo "  读不到 $ST（模块没加载？）"
	b=$(base_of)
	echo "base = $b   (容器里的 uid 0 在宿主上就是 $b)"
	if real_on; then
		echo "真 userns: 已开启"
	else
		echo "真 userns: 未开启（insmod ... real_userns=1）"
	fi
}

do_list() {
	b=$(base_of)
	n=0
	echo "base = $b"
	echo
	for d in "$DIR"/*/; do
		c=${d%/}
		[ -d "$c/rootfs/etc" ] || continue
		[ -n "$ONLY" ] && [ "$ONLY" != all ] && [ "$(basename "$c")" != "$ONLY" ] && continue
		n=$((n+1))
		R="$c/rootfs"
		nm=$(os_name "$c")
		m=$(cat "$R/$MARK" 2>/dev/null)
		own=$(ls -ldn "$R" 2>/dev/null | awk '{print $3}')
		if [ "$m" = "$b" ]; then
			st="已 relabel"
		elif [ -n "$m" ]; then
			st="标记是 $m，与当前 base $b 不符"
		else
			st="未 relabel"
		fi
		if is_running "$c"; then st="$st · 运行中"; fi
		echo "[$n] $(basename "$c")  ${nm:-unknown}"
		echo "    rootfs 属主 $own   $st"
	done
	[ "$n" = 0 ] && echo "没找到容器: $DIR"
}

do_check() {
	b=$(base_of)
	echo "base = $b"
	for d in "$DIR"/*/; do
		c=${d%/}
		[ -d "$c/rootfs/etc" ] || continue
		[ -n "$ONLY" ] && [ "$ONLY" != all ] && [ "$(basename "$c")" != "$ONLY" ] && continue
		R="$c/rootfs"
		cnt=$(left_inner "$R" "$b")
		echo "  $(basename "$c"): 还有 $cnt 项是容器内 id（该整体 +$b）"
	done
}

# 把 rootfs 里所有「容器内 id」整体平移
#
# 为什么不是只改 uid 0
#   映射是 0..65535 → base..base+65535 整段。容器里的 system(1000)、app(1001)、
#   普通用户 1000 全都在这一段里。只改 uid 0 的话，容器虽然能启动，但非 root
#   用户一读自己的文件就是 Permission denied。所以整段一起移。
#
# 性能与兼容
#   toybox 的 find 只认 -print0，不认 -uid，也不认 -exec ... {} +（前者报
#   "bad arg"，后者把 + 当路径）。所以这里只用四样东西，全部确认可用：
#     find -print0 | xargs -0 stat -c '%u %n'   扫一遍，落成「id 路径」表
#     cut -d' ' -f1 | sort -un                  取出出现过的 id
#     sed -n "s/^<id> //p"                      从表里挑出这个 id 的所有路径
#     tr '\n' '\000' | xargs -0 chown           按 NUL 分隔交给 chown
#
#   路径里可能有空格，所以 id 与路径之间只用一个空格分隔；取路径时用 sed 删
#   前缀而不是 cut 取字段，空格原样保留。
shift_ids() {
	R=$1
	dir=$2      # up = 容器 id → 宿主 id；down = 反向
	b=$3
	tmp=${TMPDIR:-/data/local/tmp}/ds-shift.$$
	top=$((b + 65536))
	n=0
	bad=0

	rm -rf "$tmp"
	mkdir -p "$tmp" || { echo 0; return 1; }

	printf '    扫描文件 ... ' >&2
	find "$R" ! -type l -print0 2>/dev/null |
		xargs -0 -r -n 200 stat -c '%u %n' 2>/dev/null > "$tmp/u.list"
	find "$R" ! -type l -print0 2>/dev/null |
		xargs -0 -r -n 200 stat -c '%g %n' 2>/dev/null > "$tmp/g.list"
	echo "ok（$(wc -l < "$tmp/u.list" | tr -d ' ') 项）" >&2

	uids=$(cut -d' ' -f1 "$tmp/u.list" 2>/dev/null | grep -v '^$' | sort -un)
	gids=$(cut -d' ' -f1 "$tmp/g.list" 2>/dev/null | grep -v '^$' | sort -un)

	# 正向必须从大到小改：先改大的，小的改成 base 后就不会再被当成源捡到
	# （0→1000 之后再处理 1000，会变成 2000，级联错误）
	if [ "$dir" = up ]; then
		uids=$(echo "$uids" | sort -rn)
		gids=$(echo "$gids" | sort -rn)
	fi

	for u in $uids; do
		if [ "$dir" = up ]; then
			[ "$u" -lt 65536 ] || continue
			nu=$((u + b))
		else
			[ "$u" -ge "$b" ] && [ "$u" -lt "$top" ] || continue
			nu=$((u - b))
		fi
		printf '    uid %s -> %s ... ' "$u" "$nu" >&2
		if sed -n "s/^$u //p" "$tmp/u.list" 2>/dev/null |
		   tr '\n' '\000' |
		   xargs -0 -r chown "$nu" 2>/dev/null; then
			echo ok >&2
		else
			echo 失败 >&2
			bad=$((bad + 1))
		fi
		n=$((n + 1))
	done

	for g in $gids; do
		if [ "$dir" = up ]; then
			[ "$g" -lt 65536 ] || continue
			ng=$((g + b))
		else
			[ "$g" -ge "$b" ] && [ "$g" -lt "$top" ] || continue
			ng=$((g - b))
		fi
		printf '    gid %s -> %s ... ' "$g" "$ng" >&2
		if sed -n "s/^$g //p" "$tmp/g.list" 2>/dev/null |
		   tr '\n' '\000' |
		   xargs -0 -r chgrp "$ng" 2>/dev/null; then
			echo ok >&2
		else
			echo 失败 >&2
			bad=$((bad + 1))
		fi
		n=$((n + 1))
	done

	rm -rf "$tmp"
	[ "$bad" != 0 ] && echo "    ! 有 $bad 组 id 没改成功" >&2

	echo "$n"
}

# 还剩多少个「没平移的容器内 id」：任何小于 base 的 id 都还没移（up 之后应全部 >= base）
left_inner() {
	R=$1
	b=$2
	find "$R" ! -type l -print0 2>/dev/null |
		xargs -0 -r -n 400 stat -c '%u %g' 2>/dev/null |
		awk -v b="$b" '$1 < b || $2 < b' | wc -l | tr -d ' '
}

relabel_one() {
	c=$1
	b=$2
	R="$c/rootfs"
	name=$(basename "$c")

	if [ -f "$R/$MARK" ] && [ "$(cat "$R/$MARK")" = "$b" ] && [ "$FORCE" != 1 ]; then
		echo "  $name: 已经是 $b，跳过（--force 可强制）"
		return 0
	fi
	if is_running "$c"; then
		echo "  $name: 容器正在运行，先停掉再改"
		return 1
	fi

	tmp=/data/local/tmp/ds-userns.$$
	mkdir -p "$tmp" || return 1

	# chown 会清掉 setuid / setgid 位，先记下来，改完再恢复
	find "$R" -perm -4000 -print > "$tmp/suid" 2>/dev/null
	find "$R" -perm -2000 -print > "$tmp/sgid" 2>/dev/null
	ns=$(wc -l < "$tmp/suid" 2>/dev/null | tr -d ' ')
	ng=$(wc -l < "$tmp/sgid" 2>/dev/null | tr -d ' ')

	echo "  $name: 开始（记下 $ns 个 setuid、$ng 个 setgid）"
	t0=$(date +%s)

	# 整段平移：容器内 0..65535 → 宿主 base..base+65535
	ids=$(shift_ids "$R" up "$b")

	# setuid 文件通常只有几十个，逐个恢复，免得再被特殊字符绊住
	while IFS= read -r f; do [ -n "$f" ] && chmod u+s "$f" 2>/dev/null; done < "$tmp/suid"
	while IFS= read -r f; do [ -n "$f" ] && chmod g+s "$f" 2>/dev/null; done < "$tmp/sgid"

	t1=$(date +%s)
	rm -rf "$tmp"

	# 复查：还有容器内 id 没平移掉就是没改干净，不写标记（避免半成品）
	left=$(left_inner "$R" "$b")
	if [ "$left" != 0 ]; then
		echo "  $name: 还有 $left 项没平移（权限或只读挂载？），标记未写"
		return 1
	fi

	echo "$b" > "$R/$MARK"
	chown "$b:$b" "$R/$MARK" 2>/dev/null

	echo "  $name: 完成，平移 $ids 组 id，用时 $((t1 - t0))s（setuid $ns / setgid $ng 已恢复）"
	return 0
}

do_relabel() {
	b=$(base_of)
	if [ "$(id -u)" != 0 ]; then
		echo "需要 root"
		exit 1
	fi
	# base 必须落在容器 id 区间之外，否则「平移后」和「还没平移」分不开
	if [ "$b" -lt 65536 ] && [ "$DS_ALLOW_SMALL_BASE" != 1 ]; then
		echo "base = $b 太小：必须 >= 65536（容器 id 是 0..65535，重叠了就分不清）"
		echo "改 /data/adb/modules/Droidspaces/userns_base 里的大数字，或 insmod 时带 userns_base="
		exit 1
	fi
	# 硬护栏：本机内核没有 CONFIG_USER_NS（unshare -U 返回 EINVAL），任何进程都不可能
	# 真的进 userns —— uid_map 恒为 "0 0 4294967295" 的宿主全映射。这时候改属主，磁盘
	# 变成 $b、进程还是 0，容器立刻读不动自己的文件（su 报 cannot set groups），
	# 还得再花几分钟 restore。real_on 只能证明钩子挂上了，不能证明有命名空间。
	if ! ns_live; then
		echo "拒绝: 没有任何进程在 user namespace 里（uid_map 全是 0 0 4294967295）。"
		echo "      本机内核未启用 CONFIG_USER_NS，改属主只会让容器读不动自己的文件。"
		echo "      确认要改就加 --force：sh userns.sh relabel all --force"
		[ "$FORCE" = 1 ] || exit 1
		echo "已带 --force，继续（后果自负）"
		echo
	fi
	if ! real_on; then
		echo "注意: 模块没以 real_userns=1 加载，这次改到的 $b 可能不是你实际会用的值"
		echo
	fi
	ok=0; bad=0
	for d in "$DIR"/*/; do
		c=${d%/}
		[ -d "$c/rootfs/etc" ] || continue
		[ -n "$ONLY" ] && [ "$ONLY" != all ] && [ "$(basename "$c")" != "$ONLY" ] && continue
		if relabel_one "$c" "$b"; then ok=$((ok+1)); else bad=$((bad+1)); fi
	done
	echo "---"
	echo "成功 $ok，失败 $bad"
	[ "$bad" = 0 ]
}

do_restore() {
	if [ "$(id -u)" != 0 ]; then echo "需要 root"; exit 1; fi
	ok=0; bad=0
	for d in "$DIR"/*/; do
		c=${d%/}
		[ -d "$c/rootfs/etc" ] || continue
		[ -n "$ONLY" ] && [ "$ONLY" != all ] && [ "$(basename "$c")" != "$ONLY" ] && continue
		if restore_one "$c"; then ok=$((ok+1)); else bad=$((bad+1)); fi
	done
	echo "---"
	echo "成功 $ok，失败 $bad"
	[ "$bad" = 0 ]
}

restore_one() {
	c=$1
	R="$c/rootfs"
	name=$(basename "$c")

	if [ ! -f "$R/$MARK" ]; then
		echo "  $name: 没有 relabel 标记，跳过"
		return 0
	fi
	b=$(tr -dc '0-9' < "$R/$MARK" 2>/dev/null)
	if [ -z "$b" ] || [ "$b" = 0 ]; then
		echo "  $name: 标记内容异常（$b），跳过"
		return 1
	fi
	if is_running "$c"; then
		echo "  $name: 容器正在运行，先停掉再改"
		return 1
	fi

	tmp=/data/local/tmp/ds-unuserns.$$
	mkdir -p "$tmp" || return 1
	find "$R" -perm -4000 -print > "$tmp/suid" 2>/dev/null
	find "$R" -perm -2000 -print > "$tmp/sgid" 2>/dev/null

	echo "  $name: 改回容器内 id（base $b）"
	t0=$(date +%s)
	ids=$(shift_ids "$R" down "$b")
	while IFS= read -r f; do [ -n "$f" ] && chmod u+s "$f" 2>/dev/null; done < "$tmp/suid"
	while IFS= read -r f; do [ -n "$f" ] && chmod g+s "$f" 2>/dev/null; done < "$tmp/sgid"
	t1=$(date +%s)
	rm -rf "$tmp"

	rm -f "$R/$MARK"
	echo "  $name: 完成，还原 $ids 组 id，用时 $((t1 - t0))s"
	return 0
}

case "$ACT" in
	status)  do_status ;;
	list|ls) do_list ;;
	check)   do_check ;;
	relabel) do_relabel ;;
	restore) do_restore ;;
	*)
		echo "用法: userns.sh {status|list|check|relabel|restore} [容器名|all] [--force]"
		exit 2 ;;
esac
