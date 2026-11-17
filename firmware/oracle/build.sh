#!/usr/bin/env bash
# Build psbt_cli (the firmware's PSBT review + signing, native, with ASan/UBSan).
set -euo pipefail
cd "$(dirname "$0")"
FW=..
WALLY=$FW/.pio/libdeps/test/libwally-core
[ -f "$WALLY/include/wally_core.h" ] || { echo "libwally missing: run 'pio pkg install -e test' in firmware/"; exit 1; }
mkdir -p out/obj
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
DEF="-DWALLY_ABI_NO_ELEMENTS"
if [ ! -f out/obj/wally.o ] || [ build.sh -nt out/obj/wally.o ]; then
    gcc -O1 -g $SAN -w $DEF -DBUILD_MINIMAL -I../fuzz/wally_config -I$FW/lib/wally/config -I$WALLY -I$WALLY/src \
        -I$WALLY/src/ccan -I$WALLY/src/secp256k1/include -c $FW/lib/wally/wally_amalgamation.c -o out/obj/wally.o
fi
APPS=$(grep -o '+<app/[a-z0-9_]*\.cpp>' $FW/platformio.ini | sort -u | sed 's/+<\(.*\)>/\1/' | grep -v qrscan)
g++ -std=gnu++17 -O1 -g $SAN -w $DEF -I$FW/include -I$WALLY/include -I$FW/src/app -o out/psbt_cli \
    psbt_cli.cpp $(for a in $APPS; do echo $FW/src/$a; done) out/obj/wally.o
echo "built out/psbt_cli"
