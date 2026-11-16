#!/usr/bin/env bash
# Build the fuzzers with GCC + ASan/UBSan and the coverage driver in driver.cpp.
# Needs libwally fetched once:  (cd .. && pio pkg install -e test)
# Usage: fuzz/build.sh            -> fuzz/out/fuzz_{psbt,ur,seedqr,address,qr}
set -euo pipefail
cd "$(dirname "$0")"
FW=..
WALLY=$FW/.pio/libdeps/test/libwally-core
[ -f "$WALLY/include/wally_core.h" ] || { echo "libwally missing: run 'pio pkg install -e test' in firmware/"; exit 1; }
OUT=out
mkdir -p $OUT/obj

CXX=${CXX:-g++}
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
COV="-fsanitize-coverage=trace-pc"
INC="-I$FW/include -I$WALLY/include -I$FW/src/app -I$FW/lib/quirc/src"
DEF="-DWALLY_ABI_NO_ELEMENTS"
WALLY_INC="-Iwally_config -I$FW/lib/wally/config -I$WALLY -I$WALLY/src -I$WALLY/src/ccan -I$WALLY/src/secp256k1/include"
OPT="-O1 -g"

objs=()
compile() {  # src flags...
    local src=$1; shift
    local o=$OUT/obj/$(echo "$src" | tr '/.' '__').o
    # libraries are cached; our own code (and harnesses that #include it) always rebuilds
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ build.sh -nt "$o" ] || [[ "$src" != *libwally* && "$src" != *lib/* ]]; then
        case $src in
            *.c) $CC $OPT $SAN "$@" $INC $DEF -w -c "$src" -o "$o" ;;
            *)   $CXX -std=gnu++17 $OPT $SAN "$@" $INC $DEF -w -c "$src" -o "$o" ;;
        esac
    fi
    objs+=("$o")
}

# Libraries the firmware uses, instrumented so the fuzzer sees their branches.
compile $FW/lib/wally/wally_amalgamation.c $COV -DBUILD_MINIMAL $WALLY_INC
for f in $FW/lib/quirc/src/*.c; do compile "$f" $COV; done
libs=("${objs[@]}")

objs=(); compile driver.cpp; driver=${objs[0]}

# The firmware's logic, as the test env builds it (no UI, no board code).
objs=()
for f in $(grep -o '+<app/[a-z0-9_]*\.cpp>' $FW/platformio.ini | sort -u | sed 's/+<\(.*\)>/\1/'); do
    [ "$f" = app/ur.cpp ] && continue   # fuzz_ur #includes it; the others link it below
    compile "$FW/src/$f" $COV
done
app=("${objs[@]}")
objs=(); compile $FW/src/app/ur.cpp $COV; ur=${objs[0]}

link() {  # name harness extra-objs...
    local name=$1 harness=$2; shift 2
    objs=(); compile "$harness" $COV
    $CXX $SAN -o $OUT/$name "${objs[0]}" "$@" "${app[@]}" "$driver" "${libs[@]}"
    echo "built $OUT/$name"
}

link fuzz_psbt    fuzz_psbt.cpp    "$ur"
link fuzz_ur      fuzz_ur.cpp
link fuzz_seedqr  fuzz_seedqr.cpp  "$ur"
link fuzz_address fuzz_address.cpp "$ur"
link fuzz_qr      fuzz_qr.cpp      "$ur"
