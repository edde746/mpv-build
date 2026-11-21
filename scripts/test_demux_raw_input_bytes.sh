#!/usr/bin/env bash
# Compile how a stream counts what it reads (stream_read_unbuffered() of
# stream/stream.c) and how the demuxer collects and publishes that count
# (update_bytes_read() and get_raw_input_bytes() of demux/demux.c, patch 0244)
# VERBATIM from a patched tree: bytes read during a low level seek show in
# demuxer-cache-state's raw-input-bytes at once, and the total a client
# samples never goes back or counts a byte twice. Pass an already-patched mpv
# tree for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0244 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
python3 "$E" symbol "$source_dir/stream/stream.c" "$workdir/stream_read_unbuffered.inc" \
    --fn stream_read_unbuffered --return int
demux="$source_dir/demux/demux.c"
inc="$workdir/demux_raw_input_bytes.inc"
python3 "$E" symbol "$demux" "$inc" --fn update_bytes_read --return void
python3 "$E" symbol "$demux" "$inc" --append --fn get_raw_input_bytes --return uint64_t
# mpv builds its own sources without -Wsign-compare.
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -pthread -Wall -Wextra -Werror -Wno-unused-parameter \
    -Wno-sign-compare -I"$workdir" "$root/scripts/test_demux_raw_input_bytes.c" -o "$workdir/test"
"$workdir/test"
