#!/usr/bin/env bash
# Compile add_packet_locked() and clear_queue() from the patched demuxer
# (demux/demux.c, patch 0208) and replay the packets a refresh seek reads
# back: a stream that was already selected must drop only what it had queued
# before and keep the first packet past it, which for a subtitle stream is its
# next line (plezy#2717). Pass an already-patched mpv tree for offline use;
# otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0208 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
demux="$source_dir/demux/demux.c"
python3 "$E" type "$demux" "$workdir/demux_refresh_types.inc" --name demux_queue --name demux_stream
python3 "$E" symbol "$demux" "$workdir/demux_refresh.inc" --fn clear_queue --return void
python3 "$E" symbol "$demux" "$workdir/demux_refresh.inc" --append --fn add_packet_locked --return void
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -I"$workdir" -I"$source_dir" "$root/scripts/test_demux_refresh.c" -o "$workdir/test" -lm
"$workdir/test"
