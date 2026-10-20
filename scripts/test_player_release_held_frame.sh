#!/usr/bin/env bash
# Compile uninit_video_chain() (player/video.c) and vo_release_frames() with
# the frame bookkeeping it shares with seeks (video/out/vo.c), patch 0139, and
# end a file whose decoder lives on in the frame the VO last showed: under
# vo_mediacodec the decoder must be gone before the next file opens one
# (plezy#2755). Pass an already-patched mpv tree for offline use; otherwise
# fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

E="$root/scripts/extract.py"
vo="$source_dir/video/out/vo.c"
types="$workdir/release_held_frame_types.inc"
inc="$workdir/release_held_frame.inc"
python3 "$E" type "$source_dir/video/out/vo.h" "$types" --member VO_CAP_ROTATE90
python3 "$E" type "$source_dir/player/core.h" "$types" --append --name playback_status
python3 "$E" symbol "$vo" "$inc" --fn forget_frames --return void
python3 "$E" symbol "$vo" "$inc" --append --fn vo_seek_reset --return void
python3 "$E" symbol "$vo" "$inc" --append --fn run_release_frames --return void
python3 "$E" symbol "$vo" "$inc" --append --fn vo_release_frames --return void
python3 "$E" symbol "$source_dir/player/video.c" "$inc" --append --fn uninit_video_chain --return void

cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    "$root/scripts/test_player_release_held_frame.c" -o "$workdir/test"
"$workdir/test"
