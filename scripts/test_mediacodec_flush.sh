#!/usr/bin/env bash
# Compile the production MediaCodec flush and input path and drive when a
# flush reaches the codec: never on a codec that has handed out no input (a
# resume seeks the decoder it has just opened), always once input has left
# it. Pass an already-patched ffmpeg tree for offline use; otherwise fetch and
# patch the pinned source in a temporary directory. No Android device or JNI
# runtime is needed: the MediaCodec wrapper is stubbed by the test.
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

# The production flush and the input side it guards, sliced out of the
# patched tree by the shared extractor: the timeout and generation shift the
# functions read, the timestamp tagging, the dequeue every input slot passes
# through, the codec flush, the entry avcodec_flush_buffers reaches, and the
# send that drops an oversized access unit through that entry. The return
# types are named so a rewritten function fails here rather than matching
# something else nearby.
E="$root/scripts/extract.py"
common="$source_dir/libavcodec/mediacodecdec_common.c"
inc="$workdir/mediacodec_flush.inc"
python3 "$E" define "$common" "$inc" \
    --name MEDIACODEC_GENERATION_SHIFT --name INPUT_DEQUEUE_TIMEOUT_US
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_tag_pts --return int64_t
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_dequeue_input --return ssize_t
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_flush_codec --return int
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_flush --return int
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_send --return int

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_flush.c"
"$workdir/test"
