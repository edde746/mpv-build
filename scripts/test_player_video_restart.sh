#!/usr/bin/env bash
# Compile the production video restart/scheduling functions, then reselect video
# with an already-running AO. Pass a patched source tree for offline use.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi
E="$root/scripts/extract.py"
video="$source_dir/player/video.c"
python3 "$E" type "$source_dir/player/core.h" "$workdir/restart_types.inc" --name playback_status
python3 "$E" symbol "$video" "$workdir/restart.inc" --fn adjust_sync --return void
python3 "$E" symbol "$video" "$workdir/restart.inc" --append --fn handle_new_frame --return void
python3 "$E" symbol "$video" "$workdir/restart.inc" --append --fn update_avsync_before_frame --return void
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    "$root/scripts/test_player_video_restart.c" -o "$workdir/test" -lm
"$workdir/test"
