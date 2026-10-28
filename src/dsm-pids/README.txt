Droidspaces PIDS 单模块包（droid_lkm_pids.ko）
=============================================

只装这一个 ko，适合"我只要 --pids-limit 生效"的场景。
完整五模块包在仓库的 dsm/ 目录。

验证环境
--------
一加 Ace 6 至尊版 PMB110 / 17.0.0.101(CNO1C10P02)
内核 6.12.69-android16-6-g242e1a07f878-ab15865446-4k
别的机型或内核请先用仓库 scripts/build-ddkk.sh 重编，见 BUILD.md。

安装
----
1. 把本目录（module.prop 这一层）整个压成 zip，
   KernelSU 管理器 → 模块 → 从压缩包安装。
2. 包内预置 no_boot，**开机不会自动加载**，手机必然正常开机。
3. KernelSU 管理器 → 模块 → Droidspaces PIDS → 执行（action.sh 默认走 status），
   或 root shell 里：

   sh /data/adb/modules/DroidspacesPids/action.sh load

4. 确认：

   dmesg | grep droid_lkm_pids | tail -12

   要有 layout verified 与 pids limit enforcement active。

确认功能层生效
--------------
cat /sys/fs/cgroup/<容器目录>/pids.stats
# refused > 0 即真的在拦 fork

脱离容器的对照实验（可选）：

su -c 'CG=/sys/fs/cgroup/t2; rmdir $CG 2>/dev/null; mkdir -p $CG; echo 5 > $CG/pids.max; echo $$ > $CG/cgroup.procs; read S < $CG/pids.stats; echo "start   $S"; i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); read C < $CG/pids.current; read S < $CG/pids.stats; echo "第${i}次 current=$C  $S"; done; rmdir $CG 2>/dev/null'

current 涨到 4 就上不去、refused 增加 = 生效。

使用
----
docker run --rm --pids-limit 256 <image>

建议 >= 256：空跑容器自身约需 68 个进程，设太小容器起不来
（表现为 docker stop 超时后被 SIGKILL）。

开关文件（模块目录下）
----------------------
no_boot            存在则开机不自动加载（本包默认存在，稳定后可删）
blocked            存在则一律不加载（守卫触发时自动生成）
allow_any_device   跳过机型锁（别随包发）
allow_any_kernel   跳过内核锁（别随包发）
allow_list         追加机型白名单，一行一个代号（别随包发）
alive_sec          存活窗口秒数，默认 120，上限 900

卸载
----
sh /data/adb/modules/DroidspacesPids/action.sh unload
# 或在 KernelSU 管理器里删除模块（uninstall.sh 会先 rmmod）

注意
----
rmmod 再 insmod 会清空 pids.max 表，测完需要重启容器让 Droidspaces 重新写入。

原理、记账点选择、已知偏差：docs/PIDS-ENFORCEMENT.md
