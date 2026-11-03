#!/system/bin/sh
# Droidspaces 容器维护: 换源 / DNS / 查看
# 用法:
#   mirror.sh list
#   mirror.sh mirror [auto|ustc|tuna|ali|huawei] [容器名]
#   mirror.sh dns    [auto|ustc|tuna|ali|huawei] [容器名]
#   mirror.sh all    [auto|ustc|tuna|ali|huawei] [容器名]
#   mirror.sh src    [容器名]        查看容器的软件源文件
#
# 两种 rootfs 存储模型都要支持:
#   A) 展开目录:  Containers/<name>/rootfs/etc      （老格式）
#   B) 镜像文件:  Containers/<name>/rootfs.img      （新格式，运行时
#                  loop 挂载到 /mnt/Droidspaces/<name>）
#   B 在容器未运行时无法读写 —— 镜像没挂上，宿主侧只是一堆二进制。
BASE=/data/local/Droidspaces/Containers
MPB=/mnt/Droidspaces
ACT=${1:-mirror}
MIR=${2:-auto}
ONLY=${3:-}
D1=223.5.5.5
D2=119.29.29.29

ubu_of(){ case "$1" in
  tuna)   echo https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports ;;
  ali)    echo https://mirrors.aliyun.com/ubuntu-ports ;;
  huawei) echo https://repo.huaweicloud.com/ubuntu-ports ;;
  *)      echo https://mirrors.ustc.edu.cn/ubuntu-ports ;;
esac }
deb_of(){ case "$1" in
  tuna)   echo https://mirrors.tuna.tsinghua.edu.cn/debian ;;
  ali)    echo https://mirrors.aliyun.com/debian ;;
  huawei) echo https://repo.huaweicloud.com/debian ;;
  *)      echo https://mirrors.ustc.edu.cn/debian ;;
esac }
py_of(){ case "$1" in
  ali)    echo https://mirrors.aliyun.com/pypi/simple ;;
  huawei) echo https://repo.huaweicloud.com/repository/pypi/simple ;;
  *)      echo https://pypi.tuna.tsinghua.edu.cn/simple ;;
esac }

val(){ sed -n "s/^$1=//p" "$2" 2>/dev/null | head -1 | tr -d '"\r'; }

# 容器的可写根目录。取不到就输出空串。
# 顺序: 展开目录 -> config 里的 name 对应的挂载点 -> config 的 rootfs_path
#        -> /proc/mounts 里名字能对上容器名的 /mnt/Droidspaces/*
ctr_root(){
  c="${1%/}"
  # A) 老格式：展开目录
  if [ -d "$c/rootfs/etc" ]; then echo "$c/rootfs"; return 0; fi
  # B) img 格式：运行时挂到 /mnt/Droidspaces/<name>
  cn=$(val name "$c/container.config")
  [ -z "$cn" ] && cn=$(basename "$c")
  for m in "$MPB/$cn" "$MPB/$(basename "$c")"; do
    if [ -d "$m/etc" ]; then echo "$m"; return 0; fi
  done
  # C) config 里 rootfs_path 指向的目录（少数情况直接是目录）
  rp=$(val rootfs_path "$c/container.config")
  if [ -n "$rp" ] && [ -d "$rp/etc" ]; then echo "$rp"; return 0; fi
  # D) 兜底：遍历挂载表，名字对得上就用（改名/多实例时救急）
  b=$(basename "$c")
  for m in $(awk -v p="$MPB/" 'index($2,p)==1{print $2}' /proc/mounts 2>/dev/null); do
    case "$m" in
      *"$b"*) [ -d "$m/etc" ] && { echo "$m"; return 0; } ;;
    esac
  done
  # E) 最后兜底：借容器进程的 root。挂载点不在当前 mount namespace 里时，
  #    走 /proc/<pid>/root 照样能读写（内核按目标进程的 ns 解析）。
  #    只对 img 容器做 —— 目录名太泛（比如 root），容易撞上无关进程
  if [ "$(ctr_state "$c")" = "img" ]; then
    for n2 in "$cn" "$b"; do
      [ -z "$n2" ] && continue
      p=$(grep -lE "(^|/)$n2(/|$)" /proc/[0-9]*/cgroup 2>/dev/null | head -1 | cut -d/ -f3)
      [ -n "$p" ] && [ -d "/proc/$p/root/etc" ] && { echo "/proc/$p/root"; return 0; }
    done
  fi
  echo ""
  return 1
}

# 拿不到 root 时的原因分类，用于给用户看的话术
ctr_state(){
  c="${1%/}"
  [ -d "$c/rootfs/etc" ] && { echo dir; return; }
  [ -f "$c/rootfs.img" ] && { echo img; return; }
  [ -d "$c/rootfs" ]     && { echo broken; return; }
  echo unknown
}

why_no_root(){  # $1=容器目录 -> 一句人话
  case "$(ctr_state "$1")" in
    img)    echo "容器未运行，rootfs.img 没 loop 挂载（启动容器后再试）" ;;
    broken) echo "rootfs 目录下没有 etc，可能是创建失败的残留" ;;
    *)      echo "目录里既没有 rootfs/ 也没有 rootfs.img" ;;
  esac
}

# 注意：调用处不要写 for c in $(containers)，未加引号的展开会按空格把
# 容器名切碎。各处循环已改为直接遍历 glob。
containers(){
  for d in "$BASE"/*/; do
    [ -d "$d" ] || continue
    c="${d%/}"; n=$(basename "$c")
    [ -n "$ONLY" ] && [ "$n" != "$ONLY" ] && continue
    R=$(ctr_root "$c")
    [ -n "$R" ] && echo "$c"
  done
}

probe(){  # $1=url -> 秒数(失败 99)
  t=$(curl -o /dev/null -s -m 4 -w "%{time_total}" "$1" 2>/dev/null)
  [ -z "$t" ] && t=99
  echo "$t"
}

pick_best(){  # $1=codename $2=distro -> 输出站名(不依赖临时文件)
  best=""; bt=99
  for m in ustc tuna ali huawei; do
    if [ "$2" = "debian" ]; then u="$(deb_of "$m")/dists/$1/Release"
    else u="$(ubu_of "$m")/dists/$1/Release"; fi
    t=$(probe "$u")
    echo "   测速 $m: ${t}s" >&2
    if [ "$(awk -v a="$t" -v b="$bt" 'BEGIN{print (a<b)?1:0}' 2>/dev/null)" = "1" ]; then
      bt="$t"; best="$m"
    fi
  done
  [ -z "$best" ] && best=ustc
  echo "$best"
}

src_of(){  # $1=rootfs 根 -> 打印源文件内容
  R="$1"; A="$R/etc/apt"; found=0
  for f in "$A/sources.list" "$A/sources.list.d/ubuntu.sources" "$A/sources.list.d/debian.sources"; do
    [ -s "$f" ] || continue
    found=1; echo "== $f =="; cat "$f"
  done
  [ "$found" = 0 ] && echo "（没找到 sources.list / *.sources，容器可能尚未初始化）"
}

do_list(){
  n=0; m=0
  for d in "$BASE"/*/; do
    [ -d "$d" ] || continue
    c="${d%/}"; nm0=$(basename "$c")
    [ -n "$ONLY" ] && [ "$nm0" != "$ONLY" ] && continue
    n=$((n+1))
    R=$(ctr_root "$c")
    if [ -n "$R" ]; then
      nm=$(val PRETTY_NAME "$R/etc/os-release"); id=$(val ID "$R/etc/os-release")
      cod=$(val VERSION_CODENAME "$R/etc/os-release"); [ -z "$cod" ] && cod=$(val UBUNTU_CODENAME "$R/etc/os-release")
      s=$(grep -m1 -vE '^[[:space:]]*(#|$)' "$R/etc/apt/sources.list" 2>/dev/null)
      [ -z "$s" ] && s=$(sed -n 's/^URIs:[[:space:]]*//p' \
              "$R/etc/apt/sources.list.d/ubuntu.sources" \
              "$R/etc/apt/sources.list.d/debian.sources" 2>/dev/null | head -1)
      echo "[$n] $nm0  ${nm:-unknown}  (${id:-?}/${cod:-?})"
      echo "    源: ${s:-（空或使用 deb822）}"
      echo "    根: $R"
    else
      case "$(ctr_state "$c")" in
        img)    m=$((m+1))
                echo "[$n] $nm0  镜像未挂载  (容器未运行)"
                echo "    源: （rootfs.img 未挂载，先启动容器再操作）" ;;
        broken) echo "[$n] $nm0  rootfs 为空  (目录异常)"
                echo "    源: （$c/rootfs 下没有 etc，可能是创建失败的残留）" ;;
        *)      echo "[$n] $nm0  无法识别  (缺 rootfs.img)"
                echo "    源: （目录里既没有 rootfs/ 也没有 rootfs.img）" ;;
      esac
    fi
  done
  [ "$n" = 0 ] && echo "没找到容器: $BASE"
  echo "---"; echo "容器总数: $n"
  [ "$m" -gt 0 ] && echo "未挂载: $m 个（容器未运行，镜像没 loop 挂载，改不了文件）"
  echo "镜像挂载点: $MPB"
}

do_src(){  # $1=容器名（函数有自己的位置参数，不能直接读脚本的 $2）
  nm="$1"
  [ -z "$nm" ] && nm="$ONLY"
  if [ -z "$nm" ]; then
    c=$(containers | head -1)
    [ -n "$c" ] && nm=$(basename "$c")
  fi
  [ -z "$nm" ] && { echo "用法: mirror.sh src <容器名>"; return 1; }
  c="$BASE/$nm"
  [ -d "$c" ] || { echo "没有这个容器: $nm（$c 不存在）"; return 1; }
  R=$(ctr_root "$c")
  if [ -z "$R" ]; then
    echo "$nm: 没找到可用的 rootfs —— $(why_no_root "$c")"
    return 1
  fi
  echo "# $nm  root=$R"
  src_of "$R"
}

do_mirror(){
  [ -n "$ONLY" ] && echo "（仅处理容器: $ONLY）"
  first=$(containers | head -1)
  if [ -z "$first" ]; then
    echo "没找到可写的容器根目录: $BASE"
    echo "已有的容器:"
    _o="$ONLY"; ONLY=""; do_list | sed -n '/^\[/p;/^    源:/p'; ONLY="$_o"
    return 1
  fi
  R0=$(ctr_root "$first")
  c0=$(val VERSION_CODENAME "$R0/etc/os-release"); [ -z "$c0" ] && c0=$(val UBUNTU_CODENAME "$R0/etc/os-release")
  i0=$(val ID "$R0/etc/os-release")
  if [ "$MIR" = "auto" ] && [ -z "$c0" ]; then
    echo "无法识别 $(basename "$first") 的版本代号，跳过测速"
    MIR=ustc
  fi

  if [ "$MIR" = "auto" ]; then
    echo "自动测速（$i0/$c0）…"
    MIR=$(pick_best "$c0" "$i0")
    echo "=> 选中: $MIR"
  fi

  UBU=$(ubu_of "$MIR"); DEB=$(deb_of "$MIR"); PY=$(py_of "$MIR")
  case "$i0" in
    debian) echo "apt : $DEB" ;;
    ubuntu) echo "apt : $UBU" ;;
    *)      echo "apt : $UBU （$i0 未识别，按 ubuntu 处理）" ;;
  esac
  echo "pip : $PY"
  echo "---"

  done_n=0; skip_n=0
  for d in "$BASE"/*/; do
    [ -d "$d" ] || continue
    c="${d%/}"; nm0=$(basename "$c")
    [ -n "$ONLY" ] && [ "$nm0" != "$ONLY" ] && continue
    R=$(ctr_root "$c")
    if [ -z "$R" ]; then
      echo "== $nm0 =="
      echo "   跳过: $(why_no_root "$c")"
      skip_n=$((skip_n+1)); continue
    fi
    id=$(val ID "$R/etc/os-release"); cod=$(val VERSION_CODENAME "$R/etc/os-release")
    [ -z "$cod" ] && cod=$(val UBUNTU_CODENAME "$R/etc/os-release")
    nm=$(val PRETTY_NAME "$R/etc/os-release")
    echo "== $nm0 : ${nm:-unknown} =="
    if [ -z "$cod" ]; then echo "   跳过: 无法识别版本代号"; skip_n=$((skip_n+1)); continue; fi

    case "$id" in
      ubuntu) DS="$R/etc/apt/sources.list.d/ubuntu.sources"
              DM="$UBU"; SEC="$UBU"
              KG="/usr/share/keyrings/ubuntu-archive-keyring.gpg"
              CP="main restricted universe multiverse" ;;
      debian) DS="$R/etc/apt/sources.list.d/debian.sources"
              DM="$DEB"; SEC="$DEB-security"
              KG="/usr/share/keyrings/debian-archive-keyring.gpg"
              CP="main contrib non-free non-free-firmware" ;;
      *)
        echo "   跳过: 未支持的发行版 $id"
        skip_n=$((skip_n+1)); continue
        ;;
    esac

    S="$R/etc/apt/sources.list"
    if [ -f "$DS" ]; then
      # deb822 布局（Debian 12+ / Ubuntu 24.04+ 默认）：只改 *.sources。
      # 同时写 sources.list 会让 apt 报
      # 「Target Packages is configured multiple times」，所以把旧文件停掉
      [ -f "$DS.bak" ] || cp "$DS" "$DS.bak" 2>/dev/null
      cat > "$DS" <<EOS
Types: deb
URIs: $DM/
Suites: $cod $cod-updates $cod-backports
Components: $CP
Signed-By: $KG

Types: deb
URIs: $SEC/
Suites: $cod-security
Components: $CP
Signed-By: $KG
EOS
      if grep -qE '^[[:space:]]*deb[[:space:]]' "$S" 2>/dev/null; then
        [ -f "$S.bak" ] || cp "$S" "$S.bak" 2>/dev/null
        printf '# 由 droidspaces webui 停用: 源已改到 %s\n' "$DS" > "$S"
      fi
      echo "   已写入 $(basename "$DS") (deb822)"
    else
      [ -f "$S" ] || : >"$S"
      [ -f "$S.bak" ] || cp "$S" "$S.bak" 2>/dev/null
      cat > "$S" <<EOS
deb $DM/ $cod $CP
deb $DM/ $cod-updates $CP
deb $DM/ $cod-backports $CP
deb $SEC/ $cod-security $CP
EOS
      echo "   已写入 sources.list ($cod)"
    fi

    cat > "$R/etc/pip.conf" <<EOS 2>/dev/null
[global]
index-url = $PY
trusted-host = $(echo "$PY" | sed 's#https\?://##; s#/.*##')
EOS
    [ -d "$R/root" ] && printf 'registry=https://registry.npmmirror.com\n' > "$R/root/.npmrc" 2>/dev/null
    echo "   已配置 pip / npm 镜像"
    done_n=$((done_n+1))
  done
  echo "---"
  echo "完成 $done_n 个，跳过 $skip_n 个。"
  [ "$done_n" -gt 0 ] && echo "进容器执行: apt update"
}

do_dns(){
  [ -n "$ONLY" ] && echo "（仅处理容器: $ONLY）"
  for d in "$BASE"/*/; do
    [ -d "$d" ] || continue
    c="${d%/}"; nm0=$(basename "$c")
    [ -n "$ONLY" ] && [ "$nm0" != "$ONLY" ] && continue
    R=$(ctr_root "$c")
    if [ -z "$R" ]; then echo "$nm0: 跳过（$(why_no_root "$c")）"; continue; fi
    F="$R/etc/resolv.conf"
    if [ -L "$F" ]; then rm -f "$F"; fi
    [ -f "$F" ] && [ ! -f "$F.bak" ] && cp "$F" "$F.bak" 2>/dev/null
    {
      echo "# set by droidspaces webui $(date '+%Y-%m-%d %H:%M')"
      echo "nameserver $D1"
      echo "nameserver $D2"
      echo "nameserver 8.8.8.8"
      echo "options timeout:2 attempts:2"
    } > "$F" 2>/dev/null
    echo "$nm0: DNS -> $D1 $D2"

    G="$R/etc/gai.conf"
    if [ ! -f "$G.bak" ] && [ -f "$G" ]; then cp "$G" "$G.bak" 2>/dev/null; fi
    grep -q "precedence ::ffff:0:0/96" "$G" 2>/dev/null || \
      printf 'precedence ::ffff:0:0/96  100\n' >> "$G" 2>/dev/null
    echo "   已设置 IPv4 优先 (gai.conf)"

    mkdir -p "$R/etc/apt/apt.conf.d" 2>/dev/null
    cat > "$R/etc/apt/apt.conf.d/99fast" <<EOS 2>/dev/null
// by droidspaces webui
Acquire::Languages "none";
Acquire::http::No-Cache "true";
Acquire::http::Pipeline-Depth "10";
Acquire::Queue-Mode "host";
Acquire::Retries "3";
Acquire::http::Timeout "15";
Acquire::ForceIPv4 "true";
EOS
    echo "   已写入 apt 加速配置 (99fast)"
  done
  echo "---"
  echo "提示: Droidspaces 启动会按容器配置重写 resolv.conf,"
  echo "      DNS 的正路是在 Droidspaces 里填 223.5.5.5,119.29.29.29"
}

case "$ACT" in
  list) do_list ;;
  src)  do_src "${3:-${2:-}}" ;;
  dns)  do_dns ;;
  all)  do_mirror; do_dns ;;
  *)    do_mirror ;;
esac
