#!/usr/bin/env bash
# Cut the rendered flight (marketing/renders/flight.py) into worldflight legs.
#   ./encode-legs.sh <wide-frames-dir> <tall-frames-dir>
# Scrub encodes: dense GOP (8 desktop, 4 phone), no audio, posters from the encoded mp4.
set -euo pipefail
WIDE="$1"; TALL="$2"; OUT="$(dirname "$0")/assets"; mkdir -p "$OUT"
LEGS=("1 120" "121 288" "289 432" "433 576" "577 696")
for i in "${!LEGS[@]}"; do
  read -r a b <<<"${LEGS[$i]}"; n=$((i + 1)); count=$((b - a + 1))
  enc() { ffmpeg -y -hide_banner -loglevel error -framerate 24 -start_number "$a" -i "$1/f%04d.png" \
    -frames:v "$count" -an -vf "format=yuv420p" -c:v libx264 -profile:v high -preset slow \
    -crf "$3" -g "$4" -keyint_min "$4" -sc_threshold 0 -movflags +faststart "$2"; }
  enc "$WIDE" "$OUT/leg$n.mp4" 20 8
  enc "$TALL" "$OUT/leg$n-m.mp4" 23 4
  ffmpeg -y -hide_banner -loglevel error -i "$OUT/leg$n.mp4" -frames:v 1 -q:v 80 "$OUT/p$n.webp"
  echo "leg$n: frames $a-$b  $(du -h "$OUT/leg$n.mp4" | cut -f1) / $(du -h "$OUT/leg$n-m.mp4" | cut -f1)"
done
