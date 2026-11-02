#!/usr/bin/env bash
# Drive the pinned FFmpeg's spdif muxer with TrueHD units across two
# seamless-branch input_timing jumps and a uint16 wrap, and check every unit
# lands whole, in order, at the IEC 61937 carrier offset its input_timing names
# (patch 0039). A minimal host configure builds just the spdif muxer (no asm,
# no programs). Pass an already patched FFmpeg tree for offline use, otherwise
# fetch the pinned Android source.
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
    # which is where the spdifenc patch lives).
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi
source_dir="$(cd "$source_dir" && pwd)"

build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$source_dir/configure" --disable-everything --disable-programs --disable-doc \
    --disable-network --disable-autodetect --disable-asm \
    --enable-muxer=spdif >"$workdir/configure.log" 2>&1) \
    || { cat "$workdir/configure.log"; exit 1; }
make -C "$build" -j"$(getconf _NPROCESSORS_ONLN)" libavformat/libavformat.a \
    libavcodec/libavcodec.a libavutil/libavutil.a \
    >"$workdir/make.log" 2>&1 || { tail -50 "$workdir/make.log"; exit 1; }

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$build" -I"$source_dir" \
    -o "$workdir/test" "$root/scripts/test_spdif_truehd_branch.c" \
    "$build/libavformat/libavformat.a" "$build/libavcodec/libavcodec.a" \
    "$build/libavutil/libavutil.a" -lm -lpthread
"$workdir/test"
