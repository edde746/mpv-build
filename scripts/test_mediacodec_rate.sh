#!/usr/bin/env bash
# Compile the production media3_rate path from the final Android ffmpeg
# series: the decoder's own operating rate after its first frame
# (mediacodecdec.c) and the host's operating-rate call it overrides
# (mediacodec.c). Pass an already patched ffmpeg tree for offline use;
# otherwise fetch and patch the pinned source in a temporary directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved ffmpeg/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi

# Sliced out of the patched tree by the shared extractor. The return types
# are named so a rewritten function fails here rather than matching
# something else nearby.
E="$root/scripts/extract.py"
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec.c" \
    "$workdir/mediacodec_rate.inc" --fn apply_media3_rate --return void
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec.c" \
    "$workdir/mediacodec_rate.inc" --append --fn mediacodec_receive_frame_cb --return int
python3 "$E" symbol "$source_dir/libavcodec/mediacodec.c" \
    "$workdir/mediacodec_rate.inc" --append --fn av_mediacodec_set_operating_rate --return int

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_rate.c" -lm
"$workdir/test"
