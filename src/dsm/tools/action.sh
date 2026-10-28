#!/system/bin/sh
# 命令行加载入口（保留备用）。
# 放在 tools/ 子目录里 —— KernelSU/Magisk 只认模块根目录的 action.sh，
# 所以管理器不会因此多出「操作」按钮，只显示 WebUI 一个。
# 用法: su -c 'sh /data/adb/modules/Droidspaces/tools/action.sh'
MODDIR=${0%/*}/..
PH_ACTION=1 exec sh "$MODDIR/post-fs-data.sh"
