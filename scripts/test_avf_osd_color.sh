#!/usr/bin/env bash
# Compile vo_avfoundation's overlay colour (the marked "avf osd color" region,
# patch 0052) VERBATIM from the final Apple series and run it against real
# CoreVideo buffers, so this needs macOS. Pass an already-patched mpv tree for
# offline use; otherwise fetch and patch the pinned source.
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
    "$workdir/avf_osd_color.inc" --start "avf osd color" \
    --require "avf_osd_color_apply"

# The VO's deployment target: CVBufferGetAttachment is deprecated from 12.
cc -O2 -std=c11 -Wall -Wextra -Werror -mmacosx-version-min=11.0 -I"$workdir" \
    -framework CoreFoundation -framework CoreVideo -framework CoreGraphics \
    -o "$workdir/test" "$root/scripts/test_avf_osd_color.c"
"$workdir/test"
