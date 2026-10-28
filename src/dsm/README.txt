Droidspaces 6.12 v1.0.20-6.12 (OnePlus PMB110 / 内核 6.12.69)
author: ai-luo
============================================================

六个模块
  droid_lkm         PID_NS / IPC_NS / SysV IPC / POSIX mqueue    → Docker 能起容器
  droid_lkm_vendor  oplus 厂商兼容 (指纹 / 参数修正)
  droid_lkm_misc    devtmpfs 设备节点 + userns 视图（本机内核无 CONFIG_USER_NS，视图不生效）
  droid_lkm_pids    pids 控制器：显示层(检查项变绿) + 功能层(--pids-limit 真的拦 fork)
  macvlan           容器独立局域网 IP  (--net=macvlan)
  ip6table_nat      IPv6 NAT           (--net=nat 的 IPv6)

真 userns / 容器独立 uid —— 本机做不到（v1.0.20 起明确标注，不再承诺）
  结论：这台 PMB110 的内核没有编译 CONFIG_USER_NS。

  证据（可复现，别再靠猜）：
    · 卸掉 droid_lkm_misc 后跑 unshare -U true → "Invalid argument"（EINVAL）。
      这对应内核 ksys_unshare() 里 #ifndef CONFIG_USER_NS return -EINVAL 那一条；
      如果是 SELinux 或权限问题，返回的会是 EACCES/EPERM，不是 EINVAL。
    · /proc/config.gz 里写的就是 "# CONFIG_USER_NS is not set"。
    · /proc/sys/user/max_user_namespaces 有值（62497）是厂商内核里的残留节点，
      与实际编不编 userns 无关 —— 这个值一直骗人，别再拿它当证据。
    · 容器跑起来后：/proc/<pid>/uid_map 是 "0 0 4294967295"（宿主恒等全映射），
      /proc/<pid>/ns/ 里没有 user。这两条才是"有没有真的进 userns"的判据。

  因此在本机：
    · 容器 root 就是宿主 root。隔离只有 mount namespace + 独立 rootfs + cap 集合；
      容器碰不到宿主 /data，是因为没被挂进去，不是因为 uid 不同。
    · WebUI 的「转换容器属主」已禁用。内核没有 userns，把 rootfs 属主改成 200000
      之后容器进程仍然是 0，容器立刻读不动自己的文件（su: cannot set groups），
      还得再花几分钟 restore 回来 —— 这一步纯亏。
    · real_userns 开关保留只为兼容旧的加载参数，开着也不会生效；
      探针显示的 "18/18" 只代表 id 翻译钩子挂上了，不代表容器拿到了命名空间。
    · App 里的「允许沙箱 / 硬件访问」能显示 enabled，靠的是模块让 unshare 探测
      返回成功 + devtmpfs 设备节点，跟 userns 是否真实存在无关。
    · App 判断能力时读 /proc/config.gz，本机这两项都是 not set。tools/fake_config.sh
      会把 CONFIG_USER_NS / CONFIG_DEVTMPFS 两行改成 y 再 bind mount 覆盖回去，
      目的是让 App 不再把 allow_sandboxing 写回 0。**它伪装的是"能力声明"，
      不是"容器真的进了 namespace"** —— 读到的 y 是假的，uid_map 仍是恒等映射。
      umount /proc/config.gz 或卸载模块即可恢复真实内容。

  想让容器真的拿到独立 uid，只有换一个 CONFIG_USER_NS=y 的内核。模块做不到：
  userns 是 capability 的作用域分层，不是"造一个结构体"就能补出来的。

  已经 relabel 过、现在属主不在 0 的，跑一次回到可用状态：
    sh /data/adb/modules/Droidspaces/userns.sh restore all

userns.sh 用法（现在只剩还原这一个还有意义）
  sh userns.sh status            模块状态 + 当前 base
  sh userns.sh list              列容器 + 属主状态
  sh userns.sh check  all        统计有多少文件不在 0 段（restore 后应为 0）
  sh userns.sh restore all       把 rootfs 属主改回 0 —— 只保留这一条

适用机器(硬锁)
  机型  ro.product.* 含 PMB110
  内核  6.12.69-*
  两者任一不符 → 脚本直接退出，一个 ko 都不加载。

  已验证的组合（只在这套上测过）:
    一加 Ace 6 至尊版 (PMB110)
    系统 17.0.0.101 (CNO1C10P02)
    内核 6.12.69-android16-6-g242e1a07f878-ab15865446-4k
  同机型但系统版本不同 = 未验证，内核代码偏移可能已变，加载前请三思。
  确需在别的机器上试:
    touch /data/adb/modules/Droidspaces/allow_any_device   跳过机型锁
    touch /data/adb/modules/Droidspaces/allow_any_kernel   跳过内核锁
    echo  <代号> >> /data/adb/modules/Droidspaces/allow_list   追加白名单
  ⚠ 这几个"放开"文件只在自己确认过的情况下建，别随包发给别人。

实测状态 (2026-10-28 / PMB110 / 6.12.69)
  [✓] Docker 容器运行 + 端口可访问
  [✓] 允许沙箱   Sandboxing: enabled
  [✓] 硬件访问   HW access: full，容器 /dev 为完整设备节点
  [✓] Macvlan    ip link add type macvlan 实测可建接口
  [✓] IPv6 NAT   ip6tables -t nat -L 实测可列表
  [✓] Process limit  droid_lkm_pids 双管齐下：
                     · 显示层 —— 内核报告 pids 控制器名，DS 检查项全绿
                     · 功能层 —— 容器内出现 pids.max / pids.current / pids.stats，
                       超限的 fork 直接返回 -EAGAIN，--pids-limit 真的生效
                       （实测 pids.stats 里 refused 计数在涨）
  [✗] 真 userns       不支持。unshare -U 返回 EINVAL，内核未编译 CONFIG_USER_NS；
                     容器 uid_map 恒为 "0 0 4294967295"，容器进程在宿主 init ns 里。
                     容器 root = 宿主 root，隔离靠 mount ns + 独立 rootfs。

安装 (5 步)
  1. KernelSU 刷入本包 → 重启。包内预置 no_boot，首次不加载任何 ko，手机正常开机
  2. 打开 本模块 → WebUI
  3. 点「加载」→ 六个模块都显示"已加载"
  4. 打开 Droidspaces → 编辑容器 → 确认「允许沙箱」「硬件访问」都能开
  5. 一切正常 → 回 WebUI 打开「开机自动加载」

  以后不用再动 userns 那个开关：本机内核没有 CONFIG_USER_NS，
  打开它不会带来隔离，只会让页面多一行看不懂的数字。

模块列表直接显示加载了什么（desc.sh）
  每次加载 / 卸载后，脚本会把 module.prop 的 description 改写成当前真实状态：
      已加载 4/6: lkm · vendor · misc · pids
  一个都没加载时显示原本的说明文字。
  KernelSU 的模块页面需要下拉刷新（或重开一次模块页）才看得到更新后的描述。
  WebUI 状态页的「模块说明」一行读的是同一个字段。

「加载」按钮的规则
  · 按「加载内容」里开着的开关执行：只开 3 个就只装 3 个
  · 6 个全关时按钮变灰，点了也不会装任何东西
  · 开关只决定"装哪些"，不会卸载已加载的；要重载请先「卸载」再「加载」
  · 「延迟加载」只对开机生效，手动点「加载」是立即执行

开关 (模块目录里的文件，WebUI 中对应滑动开关)
  no_boot        开机不自动加载（本包默认存在）
  skip_lkm       跳过 droid_lkm        PID/IPC/mqueue，Docker 靠它，别关
  skip_vendor    跳过 vendor           厂商兼容，关掉可能让 hook 变差
  skip_misc      跳过 misc             会失去 userns + devtmpfs
  skip_pids      跳过 pids 模块        Process limit 检查项变回 ✗，--pids-limit 失效
  skip_macvlan   跳过 macvlan          会失去容器独立 IP
  skip_ip6nat    跳过 ip6table_nat     会失去 IPv6 NAT
  skip_net       一次跳过上面两个网络模块

  其它:
  no_log         关闭日志记录（不写 load.log / history.log，加载照常）
  no_precheck    关闭加载前自检
  delay          内容为秒数，开机延迟加载（上限 600）
  alive_sec      存活窗口秒数，默认 120，上限 900（见"安全机制"第 4 条）
  blocked        存在则一律不加载（守卫触发时生成）

★ 网络扩展 (macvlan / ip6table_nat) —— 必须用 DDK 工具链编译
  内核开了 KCFI(clang 的控制流完整性)，模块函数的签名必须与内核编译时一致。
  用普通 clang(18) 编出来的版本，insmod 时内核会报:
    CFI failure at do_one_initcall (target: init_module+0x0 [macvlan])
    然后直接关机。
  本包内的这两个 ko 是用 DDK(clang 19 / KDIR android16-6.12) 编的，实测可加载。
  → 以后要重编这两个，必须用同一个 DDK 镜像，不能用系统 clang。

  实测结论:
    macvlan   能加载、能建接口，但这台机器的 WiFi 驱动只认一个 MAC,
              收发不通(TX 涨, RX 恒 0, ARP 永远 FAILED)。要真正使用需要 USB 有线网卡。
    ip6nat    nat 表可注册、规则可增删，地址自解析成功。

  地址仍是每次开机现查(KASLR)，模块初始化时用 KallRecon 从内核 kallsyms 表按名查。
  不需要网络扩展: touch /data/adb/modules/Droidspaces/skip_net

★ devtmpfs
  内核 CONFIG_DEVTMPFS=n 也能补：模块用 shmem 自建一个名为 "devtmpfs" 的
  文件系统并注册进内核（只注册，不起线程、不 hook、不碰 dentry）。
  注册后 /proc/filesystems 出现 "nodev devtmpfs"，Droidspaces 即认为支持；
  挂载与节点填充全部由 Droidspaces 自己完成。
  → 注意：不要给 misc 加 devtmpfs=0，那是关闭开关。

★ droid_lkm_pids（显示层 + 功能层）
  内核没编 CONFIG_CGROUP_PIDS，cgroup 控制器是编译期固定的，树外模块加不了
  真正的控制器条目。这个模块绕过去，分两层实现：

  显示层 —— Droidspaces 判断"有没有 pids 控制器"只问内核打印的两段文本：
    · /sys/fs/cgroup/cgroup.controllers （cgroup2 根）
    · /proc/cgroups                    （v1 路径）
  hook 这两个打印函数、在输出末尾补上 "pids" 名，检查项就绿了：
    加载前 cgroup.controllers = "memory"
    加载后 cgroup.controllers = "memory\npids"
    加载后 /proc/cgroups 多一行 "pids 0 1 1"

  功能层 —— 用 cgroup_add_dfl_cftypes 往 memory 子系统上挂三个文件：
    pids.max      写容器 --pids-limit 的值，读回 "max" 或数字
    pids.current  当前计数（沿祖先链累加，含后代 cgroup）
    pids.stats    subtree/strip/rewrite/fork/charged/refused/rmdir/layout 计数
  再用 kprobe 挂 cgroup_can_fork：超限直接把返回值改成 -EAGAIN，
  于是 fork 真的失败，而不是只写个数字给人看。

  验证（真机 PMB110）：
    su -c 'CG=/sys/fs/cgroup/t2; mkdir -p $CG; echo 5 > $CG/pids.max; \
           echo $$ > $CG/cgroup.procs; \
           i=0; while [ $i -lt 8 ]; do (sleep 6 &) 2>/dev/null; i=$((i+1)); \
           read C < $CG/pids.current; read S < $CG/pids.stats; \
           echo "第${i}次 current=$C  $S"; done'
    → current 涨到上限就卡住，shell 报不出来进程、停顿几秒后重来；
      pids.stats 里的 refused 每轮都在涨 —— 那就是被拦下的 fork 次数。
    → 对照实验：--pids-limit=32 时容器 docker stop 超时被 SIGKILL（饿死了），
      改 80 就能干净停止。说明限制真的落到了 fork 上。

  已知偏差：
    · 通过写 cgroup.procs 迁移进容器的进程不立即计入 pids.current（真实控制器会
      立即计入），所以 limit=5 时计数只到 4，实际能容纳 5 个。
    · --pids-limit 建议 ≥ 256。空跑的容器自身约需 68 个进程，
      设 32 会饿死容器（表现为 docker stop 超时后被 SIGKILL）。
    · 换内核要重编：模块启动时会校验 struct css_set 等结构布局，
      校验失败就自动退化成"只有显示层"，不崩，只是不限制。
    · 重载模块会清空已记录的 limit，需要重启容器让 DS 重新写 pids.max。

  它是独立模块，不挂 devtmpfs 内部 mount，所以能单独 rmmod，
  也能和已加载的 droid_lkm_misc 共存。Droidspaces 升级不需要改这个模块。

安全机制
  1. 首次安装 / 升级后默认关闭自启 —— 先手动验证，再开自启
  2. 机型 + 内核双锁：机型非 PMB110 或内核非 6.12.69-* → 直接退出
  3. bootloop 守卫（崩在 insmod 时）：insmod 前落下 insmod.inflight，成功即删；
     若某次开机崩在 insmod，下次开机检测到 → 建 blocked 并跳过
  4. 存活守卫（insmod 成功、用一会儿才崩）：加载成功留下 boot_pending，
     service.sh 等 120 秒确认系统还活着才写 .boot_ok；
     下次开机看到 boot_pending 还在、.boot_ok 不在 → 判定"加载后崩溃"，
     自动建 blocked 跳过。窗口可用 alive_sec 调（默认 120 秒）
  5. misc 默认"只注册 devtmpfs"，不起线程不 hook，不会崩

WebUI 三页
  状态  六个模块的加载状态 + 每模块开关 + 容器 uid_map / 容器 ns + 守卫状态
        + userns 面板（本机显示「未生效」，判据是容器进程真实 uid_map，
          不是模块自报的 real=1；「转换容器属主」已按此禁用）
        版本号直接读 module.prop（现在显示 v1.0.20-6.12），「模块说明」读 description
  容器  容器列表 + 软件源 + 一键换源 / DNS / 查看源文件
  日志  load.log / dmesg / 历史 / 导出 / 一键诊断

  容器状态用五条判据判定是否在跑（/proc/mounts、各进程 mountinfo、
  /proc/PID/root、各进程 cgroup、root 的 dev:inode），任一命中即"运行中"。
  真容器常挂在自己的 mount namespace 里，root shell 看不到它的挂载，
  所以 App 显示运行中而这里显示"未运行"时，以 App 为准；
  想搞清楚就把「一键诊断」导出的 ns.txt 拿出来看。

  主题：深色 / 浅色 / 跟随系统。浅色背景是纯白，跟 KernelSU 的白色顶栏同色。
  网页画不到的是 KernelSU 管理器自己的原生标题栏和 Android 系统状态栏 ——
  那条白要改的话，去管理器设置里开深色主题、或者把系统切成深色模式。

分享给别人之前
  · 只给同机型(PMB110)、同内核(6.12.69)的人。机型/内核锁会自动拦其它设备
  · 不要附带 allow_any_device / allow_any_kernel，除非你确定对方情况
  · 提醒对方：装完先别开自启，手动点「加载」，正常用 10 分钟再开
  · 出事救援顺序：安全模式 → 删 /data/adb/modules/Droidspaces → 删 blocked 后重启

排错
  ● App 里显示"硬件访问 不可用"，但 /proc/filesystems 已有 devtmpfs
    → 是 App 的缓存，清掉即可：
      su -c 'am force-stop com.droidspaces.app'
      su -c 'sed -i -e "/name=\"host_capabilities\"/d" -e "/name=\"host_capabilities_boot_id\"/d" \
             /data/data/com.droidspaces.app/shared_prefs/droidspaces_prefs.xml'

  ● WebUI 点「加载」没反应 / 只装了一部分
    → 看「日志」页输出，确认「加载内容」里对应开关是打开的

  ● Droidspaces 的 "devtmpfs support" 变成 ✗，但之前是 ✓
    → 有人写过 release=1（卸载前放掉内部 mount 的动作），那次会把 devtmpfs
      文件系统注销掉。放回去：
        su -c 'echo 0 > /sys/module/droid_lkm_misc/parameters/release'
      或者直接重启手机（模块重新加载，devtmpfs 也就回来了）。

  ● rmmod droid_lkm_misc 报 "Try again" / "Device or resource busy"
    → 正常现象：misc 持有 devtmpfs 内部 mount，那个 mount 持有模块引用。
      先放掉再卸：
        su -c 'echo 1 > /sys/module/droid_lkm_misc/parameters/release'
        su -c 'rmmod droid_lkm_misc'
      WebUI 的「卸载」已经自动做了这一步。

  ● 守卫显示"崩溃阻断"
    → 上次加载后没撑过存活窗口。先「解阻断」，再手动「加载」，观察 10 分钟；
      确认稳定后再开自启。若反复出现，把 load.log 和 history.log 发出来

  ● 容器里 --pids-limit 看起来没生效
    → 先确认 droid_lkm_pids 已加载，再看容器的 pids.stats：
        su -c 'cat /sys/fs/cgroup/<容器目录>/pids.stats'
      refused > 0 说明在拦；layout 必须是 1（结构布局校验通过）；
      refused 一直是 0 且 current 不动，多半是重载过模块、limit 表被清空了，
      重启容器让 DS 重新写一次 pids.max。

恢复
  1. 开机按住音量下 → KernelSU 安全模式
  2. 或 recovery / adb 删除 /data/adb/modules/Droidspaces
  3. 或删除 blocked 文件后重启（会重试一次）
