#!/usr/bin/env bash
# Build page_cli (the firmware's page + word reader, native) for eval_pages.py.
set -euo pipefail
cd "$(dirname "$0")"
FW=..
WALLY=$FW/.pio/libdeps/test/libwally-core
[ -f "$WALLY/include/wally_core.h" ] || { echo "libwally missing: run 'pio pkg install -e test' in firmware/"; exit 1; }
mkdir -p out/obj
DEF="-DWALLY_ABI_NO_ELEMENTS"
if [ ! -f out/obj/wally.o ] || [ build.sh -nt out/obj/wally.o ]; then
    gcc -O2 -w $DEF -DBUILD_MINIMAL -I../fuzz/wally_config -I$FW/lib/wally/config -I$WALLY -I$WALLY/src \
        -I$WALLY/src/ccan -I$WALLY/src/secp256k1/include -c $FW/lib/wally/wally_amalgamation.c -o out/obj/wally.o
fi
g++ -std=gnu++17 -O2 -w $DEF -I$FW/include -I$WALLY/include -I$FW/src/app -o out/page_cli page_cli.cpp \
    $FW/src/app/page_reader.cpp $FW/src/app/word_reader.cpp $FW/src/app/seed_create.cpp $FW/src/app/bip39_english.cpp \
    $FW/src/app/seedqr.cpp out/obj/wally.o
echo "built out/page_cli"
