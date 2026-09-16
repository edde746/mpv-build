#!/usr/bin/env bash
# Compile the marked, freestanding Dolby Vision packet filter VERBATIM from the
# ffmpeg patch's post-image, following test_mediacodec_timing.sh. Keep changes
# to this region in patch 0001: extracting it here cannot see edits in later
# patches. libavutil and libdovi are stubbed by the test.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
E="$root/scripts/extract.py"
patch_file="$root/patches/ffmpeg/pool/0001-mediacodec-dolby-vision.patch"
# The profile 7 to 8.1 conversion the filter calls; see patch 0021.
helper_file="$root/patches/ffmpeg/pool/0021-dolby-vision-convert-profile7-rpu-to-p81.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The regions are markers in the two patches' post-images, so the shared
# extractor reads them; the first and last function each region defines are
# required by name, so a region that went missing, empty or truncated fails
# here instead of compiling into something smaller than the patch says.
python3 "$E" region "$patch_file" "$workdir/mediacodec_dv_filter.inc" \
    --patch --start "mediacodec dv filter" \
    --require "dv_next_startcode" --require "dv_filter_packet"
python3 "$E" region "$helper_file" "$workdir/dovi_convert.inc" \
    --patch --start "dovi p7 conversion core" \
    --require "ff_dovi_convert_p7_rpu_to_p81"

# The converter region includes libdovi's header; the test stubs the API, so
# satisfy the include with an empty stand-in rather than a real libdovi.
mkdir -p "$workdir/stubinc/libdovi"
: > "$workdir/stubinc/libdovi/rpu_parser.h"

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -I"$workdir/stubinc" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_dv_filter.c"

"$workdir/test"
