#!/usr/bin/env bash
# Compile the production asynchronous MediaCodec output path and drive its
# end-of-stream handling: the flush-generation filter on output timestamps,
# and the EOS buffer that filter must never eat. Pass an already-patched
# ffmpeg tree for offline use; otherwise fetch and patch the pinned source in
# a temporary directory. No Android device or JNI runtime is needed: the
# MediaCodec wrapper is stubbed by the test.
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

# The production output path, sliced out of the patched tree by the shared
# extractor: the timeouts and generation shift the functions read, the output
# event codes, the timestamp tagging, the flush that advances the generation,
# the input side that queues the EOS, and the output side that must answer
# it. The return types are named so a rewritten function fails here rather
# than matching something else nearby.
E="$root/scripts/extract.py"
common="$source_dir/libavcodec/mediacodecdec_common.c"
inc="$workdir/mediacodec_eos.inc"
python3 "$E" define "$common" "$inc" \
    --name MEDIACODEC_GENERATION_SHIFT --name INPUT_DEQUEUE_TIMEOUT_US \
    --name OUTPUT_DEQUEUE_TIMEOUT_US --name OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US
python3 "$E" define "$source_dir/libavcodec/mediacodecdec_common.h" "$inc" --append \
    --name MEDIACODEC_ASYNC_FORMAT_CHANGED
python3 "$E" type "$common" "$inc" --append --member MEDIACODEC_OUTPUT_TRY_AGAIN
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_tag_pts --return int64_t
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_pts_generation --return int
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_untag_pts --return int64_t
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_dequeue_input --return ssize_t
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_dequeue_output --return int
python3 "$E" symbol "$common" "$inc" --append --fn mediacodec_dec_flush_codec --return int
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_send --return int
python3 "$E" symbol "$common" "$inc" --append --fn ff_mediacodec_dec_receive --return int

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_eos.c"
"$workdir/test"
