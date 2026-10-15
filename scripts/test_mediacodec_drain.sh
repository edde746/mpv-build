#!/usr/bin/env bash
# Compile the production MediaCodec decode path -- the receive_frame callback
# avcodec_receive_frame() reaches, the asynchronous callbacks and both
# dequeues -- and drive a drain the codec cannot finish because the host holds
# its output buffers (a player paused within the last second of a file): every
# call must hand the decoding thread back within one poll, a polled codec must
# not be asked more often than that poll allows, and the drain must still end
# once the host lets go. Pass an already-patched ffmpeg tree for offline use;
# otherwise fetch and patch the pinned source in a temporary directory. No
# Android device or JNI runtime is needed: the MediaCodec wrapper is stubbed
# by the test, and time is a virtual clock the waits advance.
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

# The production decode path, sliced out of the patched tree by the shared
# extractor: the timeouts and generation shift it reads, the timestamp
# tagging, the codec callbacks and the host wakeup, the asynchronous setup and
# wait, both dequeues, send and receive, and the receive_frame callback that
# strings them together. The return types are named so a rewritten function
# fails here rather than matching something else nearby.
E="$root/scripts/extract.py"
common="$source_dir/libavcodec/mediacodecdec_common.c"
inc="$workdir/mediacodec_drain.inc"
python3 "$E" define "$common" "$inc" \
    --name MEDIACODEC_GENERATION_SHIFT --name INPUT_DEQUEUE_TIMEOUT_US \
    --name OUTPUT_DEQUEUE_TIMEOUT_US --name OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US
python3 "$E" define "$source_dir/libavcodec/mediacodecdec_common.h" "$inc" --append \
    --name MEDIACODEC_ASYNC_FORMAT_CHANGED
python3 "$E" type "$common" "$inc" --append --member MEDIACODEC_OUTPUT_TRY_AGAIN
for fn in mediacodec_dec_tag_pts:int64_t mediacodec_dec_pts_generation:int \
          mediacodec_dec_untag_pts:int64_t mediacodec_dec_wake_host:void \
          mediacodec_dec_fail:void mediacodec_dec_on_input_available:void \
          mediacodec_dec_on_output_available:void mediacodec_dec_on_format_changed:void \
          mediacodec_dec_on_error:void mediacodec_dec_init_async:void \
          mediacodec_dec_uninit_async:void mediacodec_dec_may_block:int \
          mediacodec_dec_async_wait:int ff_mediacodec_dec_host_polls:int \
          ff_mediacodec_dec_dequeue_input:ssize_t mediacodec_dec_dequeue_output:int \
          mediacodec_dec_flush_codec:int ff_mediacodec_dec_flush:int \
          ff_mediacodec_dec_is_flushing:int ff_mediacodec_dec_send:int \
          ff_mediacodec_dec_receive:int; do
    python3 "$E" symbol "$common" "$inc" --append --fn "${fn%%:*}" --return "${fn#*:}"
done
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec.c" "$inc" --append \
    --fn mediacodec_receive_frame --return int

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_drain.c"
"$workdir/test"
