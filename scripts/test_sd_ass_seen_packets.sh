#!/usr/bin/env bash
# Compile how sd_ass tells a subtitle packet it decoded before (packet_key()
# and check_packet_seen() of sub/sd_ass.c, patch 0242) VERBATIM from a patched
# tree: a packet delivered again after a seek is seen, with a file position or
# without one (HLS). Pass an already-patched mpv tree for offline use;
# otherwise fetch and patch the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0242 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
E="$root/scripts/extract.py"
sd="$source_dir/sub/sd_ass.c"
python3 "$E" type "$sd" "$workdir/sd_ass_seen_packet_types.inc" --name seen_packet
inc="$workdir/sd_ass_seen_packets.inc"
python3 "$E" symbol "$sd" "$inc" --fn packet_key --return int64_t
python3 "$E" symbol "$sd" "$inc" --append --fn check_packet_seen --return bool
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -I"$workdir" "$root/scripts/test_sd_ass_seen_packets.c" -o "$workdir/test"
"$workdir/test"
