#!/usr/bin/env bash
# Regression test for which software-decoded frames vo_avfoundation hands the
# Dolby Vision RPU to the display layer with (plezy#2760), which ships in
# patches/mpv/pool/0004-avfoundation-video-output.patch: the patch that creates
# vo_avfoundation.m and carries every change to it.
#
# The selector is reconstructed VERBATIM from the patch's post-image (the
# marked "avf dovi profile5 rpu" region, which is deliberately freestanding C)
# and compiled against stubbed mpv, libplacebo and libavutil types. See
# test_avf_dovi_profile5_rpu.c for the scenarios.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0004-avfoundation-video-output.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The selector ships as added lines of the file the patch creates.
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/dovi_profile5_rpu.inc" \
    --start "avf dovi profile5 rpu" --patch --added-only \
    --require "find_dolby_vision_profile5_rpu"
cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_dovi_profile5_rpu.c"

"$workdir/test"
