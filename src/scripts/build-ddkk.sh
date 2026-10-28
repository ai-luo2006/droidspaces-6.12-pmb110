#!/bin/sh
# 编译主模块：droid_lkm / droid_lkm_vendor / droid_lkm_misc / droid_lkm_pids
#
# 用法: sh scripts/build-ddkk.sh [target]
#   target 默认 android16-6.12
#   可选: android12-5.10 android13-5.10 android13-5.15 android14-5.15
#         android14-6.1 android15-6.6 android16-6.12 android17-6.18
#
# VER 决定 KMI 选择与输出目录，必须与目标内核一致。
# 产物: droid_lkm/out/<target>/{droid_lkm,droid_lkm_vendor,droid_lkm_misc}.ko
set -e

TARGET=${1:-android16-6.12}
IMAGE=docker.cnb.cool/ylarod/ddk/ddk-min:${TARGET}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRCDIR="$ROOT/droid_lkm"

echo "==> target=${TARGET}"
echo "==> image=${IMAGE}"

docker run --rm \
	-e KDIR=/opt/ddk/kdir/${TARGET} \
	-e DROID_LKM_LAYOUT_FALLBACK \
	-e DROID_LKM_LAYOUT_FORCE_BUILD \
	-e DROID_LKM_LAYOUT_MANUAL \
	-e DROID_LKM_LAYOUT_TEST_BUILD_SHIFT \
	-v "$SRCDIR":/src \
	-w /src \
	"$IMAGE" \
	sh -c 'make VER="$1" clean 2>/dev/null; make VER="$1"' sh "$TARGET"

echo "==> 产物"
find "$SRCDIR/out/$TARGET" -name "*.ko"

echo "==> 拷到 dsm/ko/ 供打包"
mkdir -p "$ROOT/dsm/ko"
for k in droid_lkm droid_lkm_vendor droid_lkm_misc droid_lkm_pids; do
	[ -f "$SRCDIR/out/$TARGET/$k.ko" ] && cp "$SRCDIR/out/$TARGET/$k.ko" "$ROOT/dsm/ko/$k.ko"
done
echo "BUILD_LKM_DONE"
