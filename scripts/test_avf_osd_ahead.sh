#!/usr/bin/env bash
# Compile vo_avfoundation's subtitle wait policy (the marked "avf osd ahead
# policy" region, patch 0032) VERBATIM from the final Apple series. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 "$root/scripts/extract.py" region "$source_dir/video/out/vo_avfoundation.m" \
    "$workdir/avf_osd_ahead_policy.inc" --start "avf osd ahead policy" \
    --require "avf_osd_wait_until"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_osd_ahead.c"
"$workdir/test"
