#!/usr/bin/env bash
# Compile the production MediaCodec static HDR metadata path: the output
# format's "hdr-static-info" descriptor read into the decoder context (units,
# FFmpeg's SEI ranges, malformed descriptors refused, the copied buffer
# freed), the frame tagging (the decoder's over the container's unless the
# caller prefers the container's, one entry per type), and both frame
# wrappers that tag, with the surface wrapper's output buffer given back
# exactly once when tagging fails.
# Accept an already patched FFmpeg tree for offline use, otherwise fetch the
# pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved ffmpeg/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi

E="$root/scripts/extract.py"
common="$source_dir/libavcodec/mediacodecdec_common.c"
python3 "$E" define "$common" "$workdir/mediacodec_hdr.inc" \
    --name AMEDIAFORMAT_GET_INT32 --name MEDIACODEC_HDR_STATIC_INFO_SIZE
python3 "$E" type "$common" "$workdir/mediacodec_hdr.inc" --append \
    --member COLOR_FormatYUV420Planar
for fn in mediacodec_dec_hdr_static_info mediacodec_dec_parse_video_format \
          mediacodec_dec_prefer_packet mediacodec_dec_copy_coded mediacodec_dec_export_hdr; do
    python3 "$E" symbol "$common" "$workdir/mediacodec_hdr.inc" --append --return int --fn "$fn"
done
python3 "$E" symbol "$common" "$workdir/mediacodec_hdr.inc" --append \
    --return void --fn mediacodec_buffer_release
python3 "$E" symbol "$common" "$workdir/mediacodec_hdr.inc" --append \
    --return int --fn mediacodec_wrap_hw_buffer
python3 "$E" symbol "$common" "$workdir/mediacodec_hdr.inc" --append \
    --return int --fn mediacodec_wrap_sw_video_buffer

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_hdr.c"
"$workdir/test"
