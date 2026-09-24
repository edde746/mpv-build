#!/usr/bin/env bash
# Regression test for the Metal OSD rasterizer of vo_avfoundation
# (patches/mpv/pool/0031-avfoundation-metal-osd-rasterizer.patch).
#
# The rasterizer (the marked "avf metal osd core" region, freestanding apart
# from the sub_bitmaps fields it reads) and the CPU composition it
# replaces (draw_direct_libass_bitmap/draw_direct_bgra_bitmap and the blends
# under them, the VO's fallback) are compiled VERBATIM from the patched mpv
# source and run against each other on the host GPU. See
# test_avf_metal_osd.m for the scenarios and invariants.
#
# Needs macOS with a Metal device; without one the test reports SKIP. Pass an
# already patched mpv tree as the first argument to run offline, otherwise the
# pinned Apple source is fetched.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the pin,
    # clones the ref, proves HEAD is the pinned commit and applies the
    # resolved mpv/apple series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

vo="$source_dir/video/out/vo_avfoundation.m"
python3 "$root/scripts/extract.py" region "$vo" "$workdir/metal_osd_core.inc" \
    --start "avf metal osd core" \
    --require "avf_metal_osd_init" --require "avf_metal_osd_draw"
python3 "$root/scripts/extract.py" range "$vo" "$workdir/cpu_osd_blend.inc" \
    --first "static void blend_libass_bgra_scalar" \
    --last "static bool draw_subtitle_bitmaps_direct"

clang -O2 -std=gnu11 -fno-objc-arc -Wall -Wextra -Werror -Wno-unused-function \
    -I"$workdir" -framework Foundation -framework Metal -framework CoreVideo \
    -o "$workdir/test" "$root/scripts/test_avf_metal_osd.m"
"$workdir/test"
