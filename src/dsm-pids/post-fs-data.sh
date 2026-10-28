#!/system/bin/sh
# droid_lkm_pids 单模块包：开机加载逻辑
#
# 默认不加载（包内预置 no_boot）。删掉 no_boot 才会在开机时自动加载。
# 建议流程：先用 action.sh（或 KernelSU 的「执行」按钮）手动加载，
# 确认手机稳定后再删 no_boot 打开自启。

MODDIR=${0%/*}
KO="$MODDIR/ko/droid_lkm_pids.ko"
LOG="$MODDIR/load.log"

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# ---- 守卫：上次开机崩在 insmod，本次一律不加载 ----
if [ -f "$MODDIR/blocked" ]; then
	log "blocked present: skip"
	exit 0
fi
if [ -f "$MODDIR/insmod.inflight" ]; then
	log "last boot died in insmod: create blocked"
	: > "$MODDIR/blocked"
	rm -f "$MODDIR/insmod.inflight"
	exit 0
fi

# ---- 默认不自启 ----
if [ -f "$MODDIR/no_boot" ]; then
	log "no_boot: skip (manual load only)"
	exit 0
fi

# ---- 机型锁 ----
if [ ! -f "$MODDIR/allow_any_device" ]; then
	ok=0
	for p in ro.product.device ro.product.vendor.device ro.build.product ro.product.name; do
		case "$(getprop $p 2>/dev/null)" in
			*PMB110*) ok=1 ;;
		esac
	done
	[ -f "$MODDIR/allow_list" ] && grep -qx 'PMB110' "$MODDIR/allow_list" && ok=1
	if [ "$ok" != 1 ]; then
		log "device not allowed: skip"
		exit 0
	fi
fi

# ---- 内核锁 ----
if [ ! -f "$MODDIR/allow_any_kernel" ]; then
	case "$(uname -r)" in
		6.12.69*) ;;
		*)
			log "kernel $(uname -r) not allowed: skip"
			exit 0
			;;
	esac
fi

# ---- 加载 ----
if [ ! -f "$KO" ]; then
	log "ko missing: $KO"
	exit 0
fi

: > "$MODDIR/insmod.inflight"
: > "$MODDIR/boot_pending"
insmod "$KO" 2>>"$LOG"
rc=$?
rm -f "$MODDIR/insmod.inflight"

if [ "$rc" = 0 ]; then
	log "insmod ok"
else
	log "insmod failed rc=$rc"
fi
exit 0
