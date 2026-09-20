#!/usr/bin/env bash
# Compile pgssubdec's palette cache -- find_palette, flush_cache and the
# palette segment parser -- and drive an epoch boundary across it. Pass an
# already patched FFmpeg tree for offline use, otherwise fetch the pinned
# Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved ffmpeg/android series (series.common first,
    # which is where the palette patch lives).
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi

inc="$workdir/pgs_palette.inc"
E="$root/scripts/extract.py"

# The epoch caches the parser writes into, then its own RGBA packing and the
# colour conversion it packs, so the test measures production values and not a
# restatement of them.
python3 "$E" define "$source_dir/libavcodec/pgssubdec.c" "$inc" --name MAX_EPOCH_OBJECTS
python3 "$E" define "$source_dir/libavcodec/pgssubdec.c" "$inc" --append --name MAX_EPOCH_PALETTES
python3 "$E" define "$source_dir/libavcodec/pgssubdec.c" "$inc" --append --name MAX_OBJECT_REFS
python3 "$E" define "$source_dir/libavcodec/pgssubdec.c" "$inc" --append --name RGBA
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name SCALEBITS
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name ONE_HALF
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name FIX
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name YUV_TO_RGB1_CCIR_BT709
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name YUV_TO_RGB1_CCIR
python3 "$E" define "$source_dir/libavutil/colorspace.h" "$inc" --append --name YUV_TO_RGB2_CCIR
# The epoch caches themselves: one range, because every PGS type closes with
# `} Name;` rather than the bare `};` the type extractor anchors on.
python3 "$E" range "$source_dir/libavcodec/pgssubdec.c" "$inc" --append \
    --first 'typedef struct PGSSubObjectRef {' --last '} PGSSubContext;' --last-inclusive
python3 "$E" symbol "$source_dir/libavcodec/pgssubdec.c" "$inc" --append \
    --fn find_palette --return 'PGSSubPalette *'
python3 "$E" symbol "$source_dir/libavcodec/pgssubdec.c" "$inc" --append \
    --fn flush_cache --return void
python3 "$E" symbol "$source_dir/libavcodec/pgssubdec.c" "$inc" --append \
    --fn parse_palette_segment --return int

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_pgs_palette.c"
"$workdir/test"
