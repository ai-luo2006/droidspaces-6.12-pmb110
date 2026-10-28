#!/bin/sh
# 把 dsm/ 目录打成 KernelSU 可安装 zip
#
# 用法: sh scripts/package-ksu-zip.sh [输出路径]
# 默认输出: dist/Droidspaces-<version>.zip
#
# 注意：zip 根目录必须直接是模块内容（module.prop / post-fs-data.sh / ko/ ...），
# 不能多套一层文件夹，否则 KernelSU 装不上。
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DSM="$ROOT/dsm"
VER=$(awk -F= '$1=="version"{print $2; exit}' "$DSM/module.prop")
OUT=${1:-"$ROOT/dist/Droidspaces-${VER}.zip"}

mkdir -p "$(dirname "$OUT")"
command -v zip >/dev/null 2>&1 || { echo "需要 zip 命令"; exit 1; }

rm -f "$OUT"
(cd "$DSM" && zip -qr9 "$OUT" .)

echo "-> $OUT"
ls -la "$OUT"
unzip -l "$OUT" | head -30
echo "PACKAGE_DONE"
