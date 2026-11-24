#!/usr/bin/env bash
# Group crashing inputs by cause: sanitizer summary or failed check + first
# frame in our code. Usage: fuzz/triage.sh [name...]
cd "$(dirname "$0")"
for name in ${*:-psbt ur seedqr address qr multisig}; do
    [ -d crashes/$name ] || continue
    for c in crashes/$name/crash-*; do
        [ -e "$c" ] || continue
        log=$(ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1 ./out/fuzz_$name "$c" 2>&1)
        why=$(echo "$log" | grep -m1 -oE "SUMMARY: [A-Za-z]+: [A-Za-z-]+|FUZZ_CHECK failed: .*|==fuzz== timeout|runtime error: .*" | cut -c1-90)
        at=$(echo "$log" | grep -E "^ +#[0-9]+ " | grep -vE "libsanitizer|libc\.|driver\.cpp|fuzz_check|abort|raise|/usr/" | head -1 | grep -oE "[^ /]+\.(c|cpp|h):[0-9]+" )
        echo "$name|${why:-no crash on rerun}|$at|$c"
    done
done | sort -t'|' -k1,3 | awk -F'|' '{k=$1"|"$2"|"$3; n[k]++; if(!(k in ex)) ex[k]=$4} END {for (k in n) printf "%4d  %s  e.g. %s\n", n[k], k, ex[k]}' | sort -k2
