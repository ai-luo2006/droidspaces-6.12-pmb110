#!/bin/sh
# 用 Android DDK 工具链编译两个网络扩展模块：macvlan.ko / ip6table_nat.ko
#
# 为什么必须用 DDK：
#   目标内核 (6.12.69-android16-6-g242e1a07f878-ab15865446-4k) 开启了 KCFI，
#   模块函数的类型签名必须与内核编译时逐字节一致，否则 insmod 时内核报
#   "CFI failure at do_one_initcall" 并直接关机。
#   DDK 镜像内置匹配该内核的 clang 19 + KDIR(android16-6.12)。
#   用系统 clang 18 编译的模块 insmod 会直接关机（已踩过）。
#
# 用法: sh build-net-ddk.sh [target]
#   target 默认 android16-6.12，可选 android15-6.6 / android14-6.1 等
#
# 产物: net_macvlan/macvlan.ko, net_ip6nat/ip6table_nat.ko
set -e

TARGET=${1:-android16-6.12}
IMAGE=docker.cnb.cool/ylarod/ddk/ddk-min:${TARGET}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "==> target=${TARGET}"
echo "==> image=${IMAGE}"

for m in net_macvlan net_ip6nat; do
	echo "==> build ${m}"
	docker run --rm \
		-e KDIR=/opt/ddk/kdir/${TARGET} \
		-v "$ROOT/$m":/src \
		-w /src \
		"$IMAGE" \
		sh -c 'make -C $KDIR M=/src clean >/dev/null 2>&1; make -C $KDIR M=/src modules'
done

echo
echo "==> 产物"
ls -la "$ROOT/net_macvlan/macvlan.ko" "$ROOT/net_ip6nat/ip6table_nat.ko"
echo "==> 拷到 dsm/ko/ 供打包"
cp "$ROOT/net_macvlan/macvlan.ko" "$ROOT/dsm/ko/macvlan.ko"
cp "$ROOT/net_ip6nat/ip6table_nat.ko" "$ROOT/dsm/ko/ip6table_nat.ko"
echo "BUILD_NET_DONE"
