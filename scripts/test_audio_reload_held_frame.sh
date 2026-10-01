#!/usr/bin/env bash
# Compile reload_audio_output() and the ao filter's ao_process() from the
# patched mpv core (player/audio.c, patch 0034) and drive a reload while the
# ao filter still holds a frame meant for the old AO: the next AO must still be
# asked for (plezy#2530). Pass an already-patched mpv tree for offline use;
# otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0034 sits in series.common, so any group carries it; android is the
    # group whose AO always has a converter in front of it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
audio="$source_dir/player/audio.c"
python3 "$E" type "$source_dir/player/core.h" "$workdir/reload_held_frame_types.inc" \
    --name playback_status --name ao_chain
python3 "$E" symbol "$audio" "$workdir/reload_held_frame.inc" --fn ao_process --return void
python3 "$E" symbol "$audio" "$workdir/reload_held_frame.inc" --append --fn reload_audio_output --return void
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -I"$workdir" "$root/scripts/test_audio_reload_held_frame.c" -o "$workdir/test" -lm
"$workdir/test"
