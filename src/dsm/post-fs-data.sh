#!/system/bin/sh
# patchHarmony 6.12 loader (OnePlus 6.12.69)
# 模块: droid_lkm(pidns/ipcns) + droid_lkm_vendor + droid_lkm_misc(userns + devtmpfs 只注册)
# 开关文件(在模块目录):
#   no_boot      开机不自动加载
#   skip_misc    不加载 userns(misc)
#   no_precheck  关闭加载前自检
#   delay        内容为秒数, 开机延迟加载(上限 600)
#   blocked      存在则一律不加载
MODDIR=${0%/*}; KO="$MODDIR/ko"; LOG="$MODDIR/load.log"
MARK="$MODDIR/insmod.inflight"; BLOCK="$MODDIR/blocked"; HIS="$MODDIR/history.log"

NOLOG=0; [ -f "$MODDIR/no_log" ] && NOLOG=1
log(){ [ "$NOLOG" = 1 ] && return 0; echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$LOG" 2>/dev/null; }
say(){ log "$*"; [ -z "${PH_ACTION:-}" ] || echo "$*"; }
hist(){ [ "$NOLOG" = 1 ] && return 0; echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >>"$HIS" 2>/dev/null; }
loaded(){ grep -q "^$1 " /proc/modules 2>/dev/null; }

BOOT=1; [ -n "${PH_ACTION:-}" ] && BOOT=0

# ---------- 描述校正(无条件, 必须放在所有 exit 之前) ----------
# 卸载流程是 rmmod -> 写描述。如果 rmmod 让内核崩了、手机直接关机，第三段
# 根本没跑到，module.prop 就停在上一次的"已加载 6/6"，而实际一个都没加载。
# 下面这行每次开机(以及每次 WebUI 加载/卸载)都按 /proc/modules 的真实情况
# 重写一次，崩过的描述不会再留着骗人。
[ -f "$MODDIR/desc.sh" ] && sh "$MODDIR/desc.sh" >/dev/null 2>&1

# ---------- 首次安装: 默认关闭开机自启(安全模式) ----------
# 只在"从没有过日志"的真正首次运行时生效; 升级用户不受影响
if [ ! -f "$MODDIR/.initialized" ]; then
  : >"$MODDIR/.initialized" 2>/dev/null
  if [ ! -f "$LOG" ] && [ ! -f "$HIS" ]; then
    : >"$MODDIR/no_boot" 2>/dev/null
    log "首次安装: 已自动关闭开机自动加载(安全模式), 请先在 WebUI 手动加载验证"
  fi
fi

# ---------- bootloop 守卫 / 开关 ----------
if [ "$BOOT" = 1 ]; then
  if [ -f "$MARK" ]; then
    mv -f "$MARK" "$MODDIR/insmod.crash" 2>/dev/null; : >"$BLOCK"
    hist "BOOT BLOCKED (上次崩在 insmod)"; log "上次崩在 insmod -> BLOCK, 本次跳过"
    exit 0
  fi
  [ -f "$BLOCK" ] && { hist "BOOT SKIP (blocked)"; log "已 BLOCK, 跳过"; exit 0; }
  [ -f "$MODDIR/no_boot" ] && { hist "BOOT SKIP (no_boot)"; log "no_boot: 不自动加载"; exit 0; }
  # 上次开机加载过, 但没活到存活窗口(service.sh 会写 .boot_ok) -> 判定为加载后崩溃
  if [ -f "$MODDIR/boot_pending" ] && [ ! -f "$MODDIR/.boot_ok" ]; then
    : >"$BLOCK"
    mv -f "$MODDIR/boot_pending" "$MODDIR/load.crash" 2>/dev/null
    hist "BOOT BLOCKED (加载后未存活)"
    log "上次开机加载后崩溃 -> BLOCK, 本次跳过"
    exit 0
  fi
  rm -f "$MODDIR/.boot_ok" 2>/dev/null
fi

# ---------- 版本守卫(精确到 6.12.69) ----------
REL=$(uname -r)
if [ -f "$MODDIR/allow_any_kernel" ]; then
  say "跳过内核版本校验 (allow_any_kernel)"
else
  case "$REL" in
    6.12.69-*) : ;;
    *)
      say "内核 $REL 与目标 6.12.69-* 不符, 不加载"
      say "  确认可用后: touch $MODDIR/allow_any_kernel"
      hist "SKIP kernel=$REL"
      exit 0
      ;;
  esac
fi

# ---------- 机型守卫 ----------
if [ -f "$MODDIR/allow_any_device" ]; then
  say "跳过机型校验 (allow_any_device)"
else
  DEV=$(getprop ro.product.device 2>/dev/null)
  MOD=$(getprop ro.product.model 2>/dev/null)
  BRD=$(getprop ro.product.board 2>/dev/null)
  ODM=$(getprop ro.product.odm.device 2>/dev/null)
  VEN=$(getprop ro.product.vendor.device 2>/dev/null)
  SKU=$(getprop ro.boot.product.hardware.sku 2>/dev/null)
  DTM=$(cat /proc/device-tree/model 2>/dev/null)
  SYS=$(getprop ro.build.display.id 2>/dev/null | tr -d ' ')
  FINGER="$DEV $MOD $BRD $ODM $VEN $SKU $DTM"
  # post-fs-data 阶段属性服务可能还没完全就绪, 全空时不能当成"机型不符"误拦自己
  if [ -z "$DEV$MOD$BRD$ODM" ]; then
    say "机型属性读取失败, 跳过机型校验(本机)"
    ok=1
  else
    ok=0
    case "$FINGER" in *PMB110*) ok=1 ;; esac
  fi
  if [ "$ok" = 0 ] && [ -f "$MODDIR/allow_list" ]; then
    while IFS= read -r pat; do
      case "$pat" in ""|\#*) continue ;; esac
      case "$FINGER" in *"$pat"*) ok=1; break ;; esac
    done <"$MODDIR/allow_list"
  fi
  if [ "$ok" = 0 ]; then
    say "机型不符 ($FINGER), 不加载"
    say "  确认可用后: echo '<你的代号>' >> $MODDIR/allow_list"
    hist "SKIP device=$FINGER sys=${SYS:-?}"
    exit 0
  fi
  say "机型 $MOD ($DEV) 校验通过"
  say "系统 ${SYS:-未知} / 内核 $REL"
fi

# ---------- 延迟加载(仅开机) ----------
D=0
if [ "$BOOT" = 1 ] && [ -f "$MODDIR/delay" ]; then
  D=$(tr -dc '0-9' <"$MODDIR/delay" 2>/dev/null)
  [ -z "$D" ] && D=0
  [ "$D" -gt 600 ] && D=600
  if [ "$D" -gt 0 ]; then
    log "延迟 ${D}s 后加载"
    say "延迟 ${D}s 后加载..."
    sleep "$D"
  fi
fi

# ---------- 加载前自检(默认开) ----------
if [ ! -f "$MODDIR/no_precheck" ]; then
  bad=""
  for m in droid_lkm droid_lkm_vendor droid_lkm_misc; do
    [ -f "$KO/$m.ko" ] || bad="$bad ${m}:missing"
  done
  if [ -z "$bad" ]; then
    grep -aq "vermagic=6.12" "$KO/droid_lkm.ko" 2>/dev/null || bad="$bad vermagic-not-6.12"
  fi
  if [ -n "$bad" ]; then
    say "自检未通过:$bad -> 不加载"
    hist "PRECHECK FAIL:$bad"
    exit 0
  fi
  say "自检通过"
fi

[ "$BOOT" = 1 ] && { : >"$MARK" 2>/dev/null; sync 2>/dev/null; }

ins(){
  mod=$1; shift
  if loaded "$mod"; then say "$mod 已加载, 跳过"; return 0; fi
  [ -f "$KO/$mod.ko" ] || { say "$mod.ko 缺失"; return 1; }
  if insmod "$KO/$mod.ko" "$@" 2>>"$LOG"; then say "insmod $mod -> OK"; else say "insmod $mod -> FAILED rc=$?"; fi
}

# ---- ip6table_nat: 9 个未导出符号, 模块自解析 ----
# 新版 ko 在 init 里用 KallRecon 从内核 kallsyms 表按名字查地址，
# 不需要建参数、不需要改 kptr_restrict；只有自解析失败才回退到脚本传地址。
ins_ip6nat(){
  if loaded ip6table_nat; then say "ip6table_nat 已加载, 跳过"; return 0; fi
  [ -f "$KO/ip6table_nat.ko" ] || { say "ip6table_nat.ko 缺失, 跳过"; return 0; }
  if insmod "$KO/ip6table_nat.ko" 2>>"$LOG"; then
    say "insmod ip6table_nat -> OK (自解析)"
    return 0
  fi
  say "ip6nat 自解析失败, 回退: 脚本传地址"
  echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null
  A=""
  for s in ip6t_alloc_initial_table ip6t_register_table ip6t_unregister_table_exit ip6t_do_table xt_find_table xt_register_template xt_unregister_template nf_nat_ipv6_register_fn nf_nat_ipv6_unregister_fn; do
    v=$(grep " $s\$" /proc/kallsyms 2>/dev/null | head -1 | awk '{print $1}')
    case "$v" in
      ""|0000000000000000|00000000*)
        say "ip6nat: 取不到 $s 地址, 跳过本次加载"; return 1 ;;
    esac
    A="$A addr_$s=0x$v"
  done
  if insmod "$KO/ip6table_nat.ko" $A 2>>"$LOG"; then
    say "insmod ip6table_nat -> OK (脚本传参)"
  else
    say "insmod ip6table_nat -> FAILED rc=$?"
  fi
}

# ---- macvlan: 内核未导出 7 个符号, 模块自解析 ----
ins_macvlan(){
  if loaded macvlan; then say "macvlan 已加载, 跳过"; return 0; fi
  [ -f "$KO/macvlan.ko" ] || { say "macvlan.ko 缺失, 跳过"; return 0; }
  if insmod "$KO/macvlan.ko" 2>>"$LOG"; then
    say "insmod macvlan -> OK (自解析)"
    return 0
  fi
  say "macvlan 自解析失败, 回退: 脚本传地址"
  echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null
  A=""
  for s in dev_forward_skb __dev_forward_skb dev_mc_add_excl ip_check_defrag ndo_dflt_fdb_dump eth_header_cache eth_header_cache_update; do
    v=$(grep " $s\$" /proc/kallsyms 2>/dev/null | head -1 | awk '{print $1}')
    case "$v" in
      ""|0000000000000000|00000000*)
        say "macvlan: 取不到 $s 地址, 跳过本次加载"; return 1 ;;
    esac
    A="$A addr_$s=0x$v"
  done
  if insmod "$KO/macvlan.ko" $A 2>>"$LOG"; then
    say "insmod macvlan -> OK (脚本传参)"
  else
    say "insmod macvlan -> FAILED rc=$?"
  fi
}

# 每个模块独立开关: 在模块目录建 skip_<模块名> 即可单独跳过
#   skip_lkm / skip_vendor / skip_misc / skip_pids / skip_macvlan / skip_ip6nat
#   skip_net = 一键跳过 macvlan + ip6table_nat
[ -f "$MODDIR/skip_lkm" ] || ins droid_lkm gate=0 mqueue=1 nspid=1 nspid_append=1 skip_sysvipc=0 no_fake_ns=0 skip_do_exit=0
[ -f "$MODDIR/skip_vendor" ] || ins droid_lkm_vendor ghost=1 ghost_match=oplus_ pid_max_cap=0
if [ ! -f "$MODDIR/skip_misc" ]; then
  # 注意: 本机内核没有 CONFIG_USER_NS，real_userns 不代表隔离。
  # 它只决定两件事: (1) 剥掉 CLONE_NEWUSER 让 App 的探测过关;
  # (2) 是否武装 getuid/stat 那组 id 翻译钩子。两者都不产生命名空间。
  # 详见 CHANGELOG.md 与 README.txt 的「真 userns —— 本机做不到」一节。
  if [ -f "$MODDIR/real_userns" ]; then
    UBASE=""
    if [ -f "$MODDIR/userns_base" ]; then
      ub=$(tr -dc '0-9' < "$MODDIR/userns_base" 2>/dev/null)
      [ -n "$ub" ] && UBASE="userns_base=$ub"
    fi
    # userns_any=1: 非 root 应用的 unshare(CLONE_NEWUSER) 也回答成功，不建命名空间、
    # 不动凭据 —— 只为让 App 的能力探测过关。
    # userns_adopt 已在 v1.0.19 删除：它会在 fork/exec 时替换 cred 的 user_ns，
    # 而 cap_capable() 会遍历伪造结构的 ->parent，读到野指针直接 panic。
    # 这里不再传该参数；即使目录里残留 userns_adopt 文件也不会生效。
    # shellcheck disable=SC2086
    ins droid_lkm_misc real_userns=1 userns_any=1 $UBASE
  else
    ins droid_lkm_misc
  fi
fi
# pids 是独立模块: 显示层(补 pids 控制器名) + 功能层(挂 pids.max, 超限 fork
# 返回 -EAGAIN). 它不挂 devtmpfs 内部 mount, 所以能单独卸载,
# 也能和已加载的 droid_lkm_misc 共存
[ -f "$MODDIR/skip_pids" ] || ins droid_lkm_pids
# 网络扩展会改 kptr_restrict(安全影响很小, 重启复位)
if [ -f "$MODDIR/skip_net" ]; then
  say "skip_net: 跳过 macvlan + ip6table_nat"
else
  [ -f "$MODDIR/skip_macvlan" ] || ins_macvlan
  [ -f "$MODDIR/skip_ip6nat" ] || ins_ip6nat
fi

[ "$BOOT" = 1 ] && { rm -f "$MARK" 2>/dev/null; : >"$MODDIR/boot_pending"; sync 2>/dev/null; }

N=$(grep -c '^droid_lkm' /proc/modules 2>/dev/null)
say "完成: $N 个 droid 模块在线"
hist "$([ "$BOOT" = 1 ] && echo BOOT || echo ACTION) ok=$N/4 delay=$D ns=$(ls /proc/self/ns/ 2>/dev/null | tr '\n' ',')"

# 历史裁剪
if [ -f "$HIS" ]; then
  tail -n 200 "$HIS" >"$HIS.tmp" 2>/dev/null && mv -f "$HIS.tmp" "$HIS"
fi

if [ "$BOOT" = 0 ]; then
  say "---状态---"
  say "kernel: $REL"
  say "modules: $(grep -oE '^(droid_lkm|droid_lkm_vendor|droid_lkm_misc|macvlan|ip6table_nat) ' /proc/modules 2>/dev/null | tr -d ' ' | tr '\n' ' ')"
  say "ns: $(ls /proc/self/ns/ 2>/dev/null | tr '\n' ' ')"
  [ -e /proc/self/uid_map ] && say "uid_map: 有" || say "uid_map: 无"
  [ -f "$BLOCK" ] && say "守卫: BLOCKED" || say "守卫: 正常"
  [ -f "$MODDIR/delay" ] && say "延迟: $(cat "$MODDIR/delay" 2>/dev/null)s" || say "延迟: 关"
  [ -f "$MODDIR/no_precheck" ] && say "自检: 关" || say "自检: 开"
fi

# 把「已加载 N/6: ...」写进 module.prop，KernelSU 模块列表里直接可见
[ -f "$MODDIR/desc.sh" ] && sh "$MODDIR/desc.sh" >/dev/null 2>&1

# 让 App 读到的 CONFIG_USER_NS / CONFIG_DEVTMPFS 与模块的真实能力一致
[ -f "$MODDIR/tools/fake_config.sh" ] && sh "$MODDIR/tools/fake_config.sh" >/dev/null 2>&1
exit 0
