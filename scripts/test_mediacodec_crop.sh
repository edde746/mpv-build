#!/usr/bin/env bash
# Compile the production MediaCodec video format parser. Accept an already
# patched FFmpeg tree for offline use, otherwise fetch the pinned Android source.
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

# The format-key defines the parser reads, then the parser itself.
python3 "$root/scripts/extract.py" define "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_crop.inc" --name AMEDIAFORMAT_GET_INT32
python3 "$root/scripts/extract.py" symbol "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_crop.inc" --append --return int --fn mediacodec_dec_parse_video_format

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_crop.c"
"$workdir/test"
