#!/usr/bin/env bash
# Compile the production rendered-frame feedback ring from the final Android
# ffmpeg series: the codec-thread producer in mediacodecdec_common.c and the
# av_mediacodec_drain_rendered consumer in mediacodec.c. Pass an already
# patched ffmpeg tree for offline use; otherwise fetch and patch the pinned
# source in a temporary directory.
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

# The production ring, sliced out of the patched tree by the shared extractor:
# the codec-thread producer, then the consumer and the source query. The return
# types are named so a rewritten function fails here rather than matching
# something else nearby.
E="$root/scripts/extract.py"
python3 "$E" symbol "$source_dir/libavcodec/mediacodecdec_common.c" \
    "$workdir/mediacodec_rendered.inc" --fn mediacodec_dec_rendered_push --return int
python3 "$E" symbol "$source_dir/libavcodec/mediacodec.c" \
    "$workdir/mediacodec_rendered.inc" --append --fn av_mediacodec_drain_rendered --return int
python3 "$E" symbol "$source_dir/libavcodec/mediacodec.c" \
    "$workdir/mediacodec_rendered.inc" --append \
    --fn av_mediacodec_rendered_source --return 'const char *'

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_rendered.c"
"$workdir/test"
