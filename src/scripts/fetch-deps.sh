#!/bin/sh
# 拉取第三方依赖（KallRecon / Type_info / KernCall / HooKern）
#
# 版本在 droid_lkm/deps.lst 里钉死。若 droid_lkm/deps/ 下已有源码则无需运行。
#
# 方式 A：用 KMSDK（上游推荐）
#   需要 git SSH 权限：git@github.com:Dere3046/KMSDK.git
#
# 方式 B：手动 clone（离线可用的快照已随包附带在 droid_lkm/deps/）
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/droid_lkm/deps"
mkdir -p "$DEPS"

fetch() { # <name> <rev>
	name=$1; rev=$2
	url="https://github.com/Dere3046/$name"
	[ -d "$DEPS/$name/.git" ] || git clone "$url" "$DEPS/$name"
	git -C "$DEPS/$name" fetch --depth 1 origin "$rev" 2>/dev/null || git -C "$DEPS/$name" fetch origin
	git -C "$DEPS/$name" checkout -q "$rev"
	echo "OK $name @ $rev"
}

fetch KallRecon b258fb6
fetch Type_info  7182dae
fetch KernCall  cdec722
fetch HooKern   7a02afa
echo "FETCH_DEPS_DONE"
