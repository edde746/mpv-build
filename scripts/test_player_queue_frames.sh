#!/usr/bin/env bash
# Compile the core's frame-queue gate and frame-duration code from the Android
# mpv series (player/video.c) and drive them the way vo_mediacodec meets them:
# whether a decoded frame may go to the VO before the next one is decoded,
# whether the decoder keeps being asked for that next one meanwhile, and what
# duration a frame queued alone gets. Pass an already-patched mpv tree for
# offline use; otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
video="$source_dir/player/video.c"
types="$workdir/queue_frames_types.inc"
inc="$workdir/queue_frames.inc"
python3 "$E" type "$source_dir/video/out/vo.h" "$types" --member VO_CAP_ROTATE90
python3 "$E" type "$source_dir/common/common.h" "$types" --append --name video_sync
python3 "$E" type "$source_dir/player/core.h" "$types" --append --name playback_status
python3 "$E" type "$source_dir/player/core.h" "$types" --append --name frame_info
python3 "$E" type "$video" "$types" --append --member VD_ERROR
python3 "$E" symbol "$video" "$inc" --fn use_video_lookahead --return bool
python3 "$E" symbol "$video" "$inc" --append --fn get_req_frames --return int
python3 "$E" symbol "$video" "$inc" --append --fn needs_new_frame --return bool
python3 "$E" symbol "$video" "$inc" --append --fn add_new_frame --return void
python3 "$E" symbol "$video" "$inc" --append --fn get_queue_frames --return int
python3 "$E" symbol "$video" "$inc" --append --fn have_new_frame --return bool
python3 "$E" symbol "$video" "$inc" --append --fn video_output_image --return int
python3 "$E" symbol "$video" "$inc" --append --fn calculate_frame_duration --return void
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -I"$workdir" \
    "$root/scripts/test_player_queue_frames.c" -o "$workdir/test" -lm
"$workdir/test"
