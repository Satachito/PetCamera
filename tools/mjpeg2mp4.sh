#!/usr/bin/env bash
# Convert a Tab5 pet-camera clip to MP4 at the right speed.
#
# A .mjpeg file is just JPEG frames end to end: it carries no frame rate and no
# duration. Every player therefore guesses, and ffmpeg's guess is 25 fps — so a
# clip recorded at 5 fps plays five times too fast and reports a fifth of its
# real length. The fix is -framerate on the INPUT, before -i; putting it after
# only relabels the output and keeps the wrong timing.
set -euo pipefail

FPS="${PETCAM_CLIP_FPS:-5}"
CRF="${CRF:-28}"

if [ $# -lt 1 ]; then
    echo "usage: $0 <clip.mjpeg> [output.mp4]" >&2
    echo "  PETCAM_CLIP_FPS=5  recording rate the clip was made at" >&2
    echo "  CRF=28             quality: lower is better and larger" >&2
    exit 1
fi

IN="$1"
OUT="${2:-${IN%.mjpeg}.mp4}"

ffmpeg -hide_banner -loglevel error \
    -framerate "$FPS" -i "$IN" \
    -c:v libx264 -preset slow -crf "$CRF" -pix_fmt yuv420p \
    -movflags +faststart \
    -y "$OUT"

echo "$OUT"
ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$OUT" |
    awk '{printf "  %.1f seconds\n", $1}'
