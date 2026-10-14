#!/usr/bin/env bash
# Regression test for the frame-space OSD geometry used by the PiP compositing
# path, which ships in patches/mpv/pool/0004-avfoundation-video-output.patch:
# the patch that creates vo_avfoundation.m and carries every change to it.
#
# The geometry constructor and the subtitle bounding-box mapping are
# reconstructed VERBATIM from the patch's post-image (the marked "avf
# frame-space osd core" region, which is deliberately freestanding C) and
# compiled into a harness that models the letterboxed window-space geometry they
# replace, libass line placement, and the composite path vo_avfoundation then
# takes. See test_avf_pip_frame_space_osd.c for the scenarios and invariants.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0004-avfoundation-video-output.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The frame-space core ships as added lines of the file the patch creates.
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/frame_space_core.inc" \
    --start "avf frame-space osd core" --patch --added-only \
    --require "avf_frame_space_geometry" --require "avf_map_subtitle_rect"
cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_pip_frame_space_osd.c"

"$workdir/test"
