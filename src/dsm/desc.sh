#!/system/bin/sh
# 把 module.prop 的 description 改写成「当前真正加载了什么」，
# 这样 KernelSU 的模块列表里不用点开就能看到：
#     已加载 4/6: lkm · vendor · misc · pids
# 一个都没加载时回退成原本的说明文字，避免列表里出现空的/误导的描述。
#
# 调用点：post-fs-data.sh（加载完）、uninstall.sh（卸载完）、WebUI 的加/卸载按钮。
# 只有 description 一行会被改写，id / name / version / versionCode / author 原样保留。

MODDIR=${0%/*}
PROP="$MODDIR/module.prop"
BASE='6 模块仅在 PMB110 / 6.12.69 真机验证过。WebUI: 状态/容器/日志，含每模块开关、卸载确认、机型与内核守卫、bootloop 守卫。默认不自动加载。'

[ -f "$PROP" ] || exit 0

list=""; n=0
for m in droid_lkm droid_lkm_vendor droid_lkm_misc droid_lkm_pids macvlan ip6table_nat; do
  grep -q "^${m} " /proc/modules 2>/dev/null || continue
  case "$m" in
    droid_lkm)          s=lkm;;
    droid_lkm_vendor)   s=vendor;;
    droid_lkm_misc)     s=misc;;
    droid_lkm_pids)     s=pids;;
    macvlan)            s=macvlan;;
    ip6table_nat)       s=ip6nat;;
    *)                  s=$m;;
  esac
  if [ -n "$list" ]; then list="$list · $s"; else list=$s; fi
  n=$((n+1))
done

if [ "$n" -gt 0 ]; then
  desc="已加载 $n/6: $list"
else
  desc="$BASE"
fi

# 先写临时文件再 mv：同一分区内 mv 是 rename，管理器不会读到写了一半的文件
grep -v '^description=' "$PROP" >"$PROP.tmp" 2>/dev/null || exit 0
echo "description=$desc" >>"$PROP.tmp" 2>/dev/null || exit 0
mv -f "$PROP.tmp" "$PROP" 2>/dev/null
exit 0
