#!/system/bin/sh
# 卸载模块时先 rmmod，避免残留 hook
rmmod droid_lkm_pids 2>/dev/null
exit 0
