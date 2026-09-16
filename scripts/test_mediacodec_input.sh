#!/usr/bin/env bash
# Compile the production MediaCodec input sizing and packet submission path.
# Pass an already-patched ffmpeg tree for offline use; otherwise fetch and patch
# the pinned source in a temporary directory. No Android device or JNI runtime
# is needed: the MediaCodec wrapper is stubbed by the test.
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

# The production input sizing and submission path, sliced out of the patched
# tree by the shared extractor. The return types are named so a rewritten
# function fails here rather than matching something else nearby.
E="$root/scripts/extract.py"
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec.c" \
    "$workdir/mediacodec_input.inc" --fn video_max_input_size --return int
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_input.inc" --append \
    --fn ff_mediacodec_dec_dequeue_input --return ssize_t
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_input.inc" --append --fn mediacodec_dec_tag_pts --return int64_t
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_input.inc" --append --fn ff_mediacodec_dec_send --return int

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_input.c"
"$workdir/test"
