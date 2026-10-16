#!/usr/bin/env bash
# Compile mpv's decoder filter step and how mpv registers its decoder wakeup
# against FFmpeg's production MediaCodec decode path -- the receive_frame
# callback avcodec_receive_frame() reaches, the asynchronous callbacks, both
# dequeues, the busy answer and the buffer release -- and drive a paused
# player, which holds the codec's output buffers mid-file and at the end of
# the file (a drain the codec cannot finish): once the codec can make no
# progress the decoder must not be asked again until the player gives a
# buffer back or seeks, every call must hand the decoding thread back within
# one poll, a codec still working or still taking input must be asked until
# its output comes, and the drain must end once the player resumes. Pass an
# already-patched mpv tree and ffmpeg tree for offline use; otherwise fetch
# and patch the pinned Android sources in a temporary directory. No Android
# device or JNI runtime is needed: the MediaCodec wrapper is stubbed by the
# test, and time is a virtual clock the waits advance.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mpv_dir="${1:-}"
ffmpeg_dir="${2:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The pin contract lives in scripts/patches.py: it resolves the
# override-aware pin, clones the ref, proves HEAD is the pinned commit and
# applies the resolved android series.
if [[ -z "$mpv_dir" ]]; then
    mpv_dir="$workdir/mpv"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$mpv_dir")
fi
if [[ -z "$ffmpeg_dir" ]]; then
    ffmpeg_dir="$workdir/ffmpeg"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$ffmpeg_dir")
fi

# Sliced out of the patched trees by the shared extractor. From mpv: the
# filter step every lavc decoder runs and its state, the wakeup the decoder
# calls, and the registration in the video decoder's init. From FFmpeg: the
# timeouts, park time and generation shift the decode path reads, the
# timestamp tagging, the codec callbacks and the host wakeup, the asynchronous
# setup and wait, the busy answer and the release that wakes a parked host,
# the release on a frame's last reference, both dequeues, flush, send and
# receive, the receive_frame callback that strings them together, and the
# two releases a video output gives a shown frame back through. The return
# types are named so a rewritten function fails here rather than matching
# something else nearby.
E="$root/scripts/extract.py"
host="$workdir/mediacodec_paused_host.inc"
python3 "$E" type "$mpv_dir/filters/f_decoder_wrapper.h" "$host" \
    --name lavc_busy_policy --name lavc_state
python3 "$E" symbol "$mpv_dir/filters/f_decoder_wrapper.c" "$host" --append \
    --fn lavc_process --return void
python3 "$E" symbol "$mpv_dir/video/decode/vd_lavc.c" "$host" --append \
    --fn decoder_wakeup --return void
python3 "$E" range "$mpv_dir/video/decode/vd_lavc.c" "$workdir/mediacodec_paused_register.inc" \
    --first "av_mediacodec_set_wakeup_callback(avctx, decoder_wakeup, vd)" \
    --last "// Sometimes, the first packet contains"

common="$ffmpeg_dir/libavcodec/mediacodecdec_common.c"
dec="$workdir/mediacodec_paused_decoder.inc"
python3 "$E" define "$common" "$dec" \
    --name MEDIACODEC_GENERATION_SHIFT --name INPUT_DEQUEUE_TIMEOUT_US \
    --name OUTPUT_DEQUEUE_TIMEOUT_US --name OUTPUT_DEQUEUE_BLOCK_TIMEOUT_US \
    --name MEDIACODEC_PARK_US
python3 "$E" define "$ffmpeg_dir/libavcodec/mediacodecdec_common.h" "$dec" --append \
    --name MEDIACODEC_ASYNC_FORMAT_CHANGED
python3 "$E" type "$common" "$dec" --append --member MEDIACODEC_OUTPUT_TRY_AGAIN
for fn in mediacodec_dec_tag_pts:int64_t mediacodec_dec_pts_generation:int \
          mediacodec_dec_untag_pts:int64_t mediacodec_dec_wake_host:void \
          mediacodec_dec_fail:void mediacodec_dec_on_input_available:void \
          mediacodec_dec_on_output_available:void mediacodec_dec_on_format_changed:void \
          mediacodec_dec_on_error:void mediacodec_dec_init_async:void \
          mediacodec_dec_uninit_async:void mediacodec_dec_may_block:int \
          mediacodec_dec_async_wait:int ff_mediacodec_dec_host_polls:int \
          ff_mediacodec_dec_busy:void ff_mediacodec_dec_released:void \
          mediacodec_buffer_release:void \
          ff_mediacodec_dec_dequeue_input:ssize_t mediacodec_dec_dequeue_output:int \
          mediacodec_dec_flush_codec:int ff_mediacodec_dec_flush:int \
          ff_mediacodec_dec_is_flushing:int ff_mediacodec_dec_send:int \
          ff_mediacodec_dec_receive:int; do
    python3 "$E" symbol "$common" "$dec" --append --fn "${fn%%:*}" --return "${fn#*:}"
done
python3 "$E" symbol "$ffmpeg_dir/libavcodec/mediacodecdec.c" "$dec" --append \
    --fn mediacodec_receive_frame --return int
python3 "$E" symbol "$ffmpeg_dir/libavcodec/mediacodec.c" "$dec" --append \
    --fn av_mediacodec_set_wakeup_callback --fn av_mediacodec_release_buffer \
    --fn av_mediacodec_render_buffer_at_time --return int

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_paused.c"
"$workdir/test"
