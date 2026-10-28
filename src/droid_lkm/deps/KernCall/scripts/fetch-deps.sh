#!/bin/sh
set -e

cd "$(dirname "$0")/.."

SDKREV=$(cat .sdk-version)

if [ ! -d .sdk/.git ]; then
	rm -rf .sdk
	git clone https://github.com/Dere3046/KMSDK.git .sdk
fi

case $SDKREV in
[0-9a-f]*)
	git -C .sdk fetch origin 2>/dev/null || true
	git -C .sdk checkout "$SDKREV"
	;;
esac

exec .sdk/scripts/sdk install
