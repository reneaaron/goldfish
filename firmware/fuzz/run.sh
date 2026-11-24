#!/usr/bin/env bash
# Run fuzzers in parallel, restarting after each crash.
# Usage: fuzz/run.sh [seconds] [names...]    default: 600s, all fuzzers
# Crashing inputs land in fuzz/crashes/<name>/, logs in fuzz/out/<name>.log.
set -uo pipefail
cd "$(dirname "$0")"
SECS=${1:-600}; shift || true
NAMES=${*:-psbt ur seedqr address qr multisig}
mkdir -p crashes out

run() {
    local name=$1 end=$(( $(date +%s) + SECS )) i=0
    mkdir -p corpus/$name crashes/$name
    while [ "$(date +%s)" -lt "$end" ]; do
        local left=$(( end - $(date +%s) ))
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1 UBSAN_OPTIONS=abort_on_error=1:print_stacktrace=1 \
            ./out/fuzz_$name corpus/$name -t "$left" -max_len 16384 -seed $((RANDOM + i)) >>out/$name.log 2>&1
        for c in corpus/$name/crash-*; do [ -e "$c" ] && mv "$c" crashes/$name/; done
        i=$((i + 1))
    done
    echo "$name: $(ls crashes/$name | wc -l) crashing inputs, corpus $(ls corpus/$name | wc -l)"
}

for n in $NAMES; do (run "$n" 2>/dev/null) & done
wait
