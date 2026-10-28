#!/usr/bin/env bash
set -e

TESTDIR="$(cd "$(dirname "$0")" && pwd)"
MODROOT="$TESTDIR/.."
OUT="$TESTDIR/out"
TGT="${TGT:-android16-6.12}"
IMG="${IMG:-Image-android16-6.12}"
REF="${REF:-$HOME/code/KernGKI/images}"

mkdir -p "$OUT"

echo "== build kerncall =="
docker run --rm \
	-e KDIR="/opt/ddk/kdir/$TGT" \
	-v "$MODROOT":/src:ro \
	-v "$OUT":/out \
	-w /src \
	"docker.cnb.cool/ylarod/ddk/ddk-min:$TGT" \
	sh -c "tar --exclude='./out' --exclude='*/.git' -cf - . | tar -xf - -C /out && cd /out && make clean 2>/dev/null; make"

echo "== build init =="
aarch64-linux-gnu-gcc -static -O2 -o "$OUT/init" "$TESTDIR/init.c"

echo "== build initramfs =="
rm -rf "$OUT/rootfs"
mkdir -p "$OUT/rootfs"
cp "$OUT/init" "$OUT/rootfs/init"
cp "$OUT/kerncall.ko" "$OUT/rootfs/kerncall.ko"
(cd "$OUT/rootfs" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9) > "$OUT/initramfs.img"

echo "== boot qemu =="
exec qemu-system-aarch64 \
	-machine virt -cpu max -m 1G -smp 4 \
	-kernel "$REF/$IMG" \
	-initrd "$OUT/initramfs.img" \
	-nographic -no-reboot \
	-append "console=ttyAMA0 loglevel=8 rdinit=/init"
