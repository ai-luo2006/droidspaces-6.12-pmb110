#!/system/bin/sh
# Droidspaces 容器维护: 换源 / DNS / 查看
# 用法:
#   mirror.sh list
#   mirror.sh mirror [auto|ustc|tuna|ali|huawei]   (默认 auto: 自动测速选最快)
#   mirror.sh dns
#   mirror.sh all    [auto|ustc|tuna|ali|huawei]
BASE=/data/local/Droidspaces/Containers
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

val(){ sed -n "s/^$1=//p" "$2" 2>/dev/null | head -1 | tr -d '"'; }

# 注意：调用处不要写 for c in $(containers)，未加引号的展开会按空格把
# 容器名切碎。各处循环已改为直接遍历 glob。
containers(){
  for d in "$BASE"/*/; do
    n=$(basename "${d%/}")
    [ -n "$ONLY" ] && [ "$n" != "$ONLY" ] && continue
    [ -d "${d%/}/rootfs/etc" ] && echo "${d%/}"
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

do_list(){
  n=0
  for d in "$BASE"/*/; do
    c=${d%/}
    [ -d "$c/rootfs/etc" ] || continue
    [ -n "$ONLY" ] && [ "$(basename "$c")" != "$ONLY" ] && continue
    n=$((n+1)); R="$c/rootfs"
    nm=$(val PRETTY_NAME "$R/etc/os-release"); id=$(val ID "$R/etc/os-release")
    cod=$(val VERSION_CODENAME "$R/etc/os-release"); [ -z "$cod" ] && cod=$(val UBUNTU_CODENAME "$R/etc/os-release")
    s=$(head -1 "$R/etc/apt/sources.list" 2>/dev/null)
    echo "[$n] $(basename "$c")  ${nm:-unknown}  (${id:-?}/${cod:-?})"
    echo "    源: ${s:-（空或使用 deb822）}"
  done
  [ "$n" = 0 ] && echo "没找到容器: $BASE"
  echo "---"; echo "容器总数: $n"
}

do_mirror(){
  [ -n "$ONLY" ] && echo "（仅处理容器: $ONLY）"
  first=$(containers | head -1)
  if [ -z "$first" ]; then echo "没找到容器: $BASE"; return 1; fi
  c0=$(val VERSION_CODENAME "$first/rootfs/etc/os-release"); [ -z "$c0" ] && c0=$(val UBUNTU_CODENAME "$first/rootfs/etc/os-release")
  i0=$(val ID "$first/rootfs/etc/os-release")

  if [ "$MIR" = "auto" ]; then
    echo "自动测速（$i0/$c0）…"
    MIR=$(pick_best "$c0" "$i0")
    echo "=> 选中: $MIR"
  fi

  UBU=$(ubu_of "$MIR"); DEB=$(deb_of "$MIR"); PY=$(py_of "$MIR")
  echo "apt : $UBU"
  echo "pip : $PY"
  echo "---"

  for d in "$BASE"/*/; do
    c=${d%/}
    [ -d "$c/rootfs/etc" ] || continue
    [ -n "$ONLY" ] && [ "$(basename "$c")" != "$ONLY" ] && continue
    R="$c/rootfs"
    id=$(val ID "$R/etc/os-release"); cod=$(val VERSION_CODENAME "$R/etc/os-release")
    [ -z "$cod" ] && cod=$(val UBUNTU_CODENAME "$R/etc/os-release")
    nm=$(val PRETTY_NAME "$R/etc/os-release")
    echo "== $(basename "$c") : ${nm:-unknown} =="
    if [ -z "$cod" ]; then echo "   跳过: 无法识别版本代号"; continue; fi

    case "$id" in
      ubuntu)
        NF="$R/etc/apt/sources.list.d/ubuntu.sources"
        if [ -f "$NF" ]; then
          [ -f "$NF.bak" ] || cp "$NF" "$NF.bak" 2>/dev/null
          cat > "$NF" <<EOS
Types: deb
URIs: $UBU/
Suites: $cod $cod-updates $cod-backports
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: $UBU/
Suites: $cod-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOS
          echo "   已写入 ubuntu.sources (deb822)"
        fi
        S="$R/etc/apt/sources.list"
        [ -f "$S" ] || : >"$S"
        [ -f "$S.bak" ] || cp "$S" "$S.bak" 2>/dev/null
        cat > "$S" <<EOS
deb $UBU/ $cod main restricted universe multiverse
deb $UBU/ $cod-updates main restricted universe multiverse
deb $UBU/ $cod-backports main restricted universe multiverse
deb $UBU/ $cod-security main restricted universe multiverse
EOS
        echo "   已写入 sources.list ($cod)"
        ;;
      debian)
        S="$R/etc/apt/sources.list"
        [ -f "$S" ] || : >"$S"
        [ -f "$S.bak" ] || cp "$S" "$S.bak" 2>/dev/null
        cat > "$S" <<EOS
deb $DEB/ $cod main contrib non-free non-free-firmware
deb $DEB/ $cod-updates main contrib non-free non-free-firmware
deb $DEB/ $cod-backports main contrib non-free non-free-firmware
deb $DEB-security/ $cod-security main contrib non-free non-free-firmware
EOS
        echo "   已写入 sources.list ($cod)"
        ;;
      *)
        echo "   跳过: 未支持的发行版 $id"
        continue
        ;;
    esac

    cat > "$R/etc/pip.conf" <<EOS 2>/dev/null
[global]
index-url = $PY
trusted-host = $(echo "$PY" | sed 's#https\?://##; s#/.*##')
EOS
    [ -d "$R/root" ] && printf 'registry=https://registry.npmmirror.com\n' > "$R/root/.npmrc" 2>/dev/null
    echo "   已配置 pip / npm 镜像"
  done
  echo "---"; echo "完成。进容器执行: apt update"
}

do_dns(){
  [ -n "$ONLY" ] && echo "（仅处理容器: $ONLY）"
  for d in "$BASE"/*/; do
    c=${d%/}
    [ -d "$c/rootfs/etc" ] || continue
    [ -n "$ONLY" ] && [ "$(basename "$c")" != "$ONLY" ] && continue
    R="$c/rootfs"
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
    echo "$(basename "$c"): DNS -> $D1 $D2"

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
EOS
    echo "   已写入 apt 加速配置 (99fast)"
  done
  echo "---"
  echo "提示: Droidspaces 启动会按容器配置重写 resolv.conf,"
  echo "      DNS 的正路是在 Droidspaces 里填 223.5.5.5,119.29.29.29"
}

case "$ACT" in
  list) do_list ;;
  dns)  do_dns ;;
  all)  do_mirror; do_dns ;;
  *)    do_mirror ;;
esac
