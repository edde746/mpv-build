#!/usr/bin/env bash
# Compile the player's AO event handling (player/audio.c, patch 0040):
# handle_ao_events(), fall_back_to_pcm() and reload_audio_output(), and drive a
# passthrough output failing under the chain it feeds, under a chain it
# outlived, and together with a plain reload. Pass an already-patched mpv tree
# for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0040 sits in series.apple: the outputs that report the failure are Apple's.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi
E="$root/scripts/extract.py"
audio="$source_dir/player/audio.c"
python3 "$E" type "$source_dir/player/core.h" "$workdir/passthrough_fallback_types.inc" \
    --name playback_status --name seek_precision --name ao_chain
python3 "$E" type "$source_dir/audio/out/ao.h" "$workdir/passthrough_fallback_types.inc" --append \
    --member AO_EVENT_RELOAD
python3 "$E" symbol "$audio" "$workdir/passthrough_fallback.inc" --fn fall_back_to_pcm --return bool
python3 "$E" symbol "$audio" "$workdir/passthrough_fallback.inc" --append --fn reload_audio_output --return void
python3 "$E" symbol "$audio" "$workdir/passthrough_fallback.inc" --append --fn handle_ao_events --return void
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -I"$workdir" "$root/scripts/test_audio_passthrough_fallback.c" -o "$workdir/test"
"$workdir/test"
