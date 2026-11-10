#!/usr/bin/env bash
# Build the patched FFmpeg's http and tcp protocols for the host (a minimal
# configure: no codecs, no formats, no asm, no programs) and read a resource
# through them from an HTTP/1.1 server the test runs on loopback: persistent
# range requests (patch 0080) against servers that keep connections alive,
# close them, ignore Range, send chunked bodies, redirect, cut a reply short
# or stall, and the unchanged wire behaviour without the new options (which
# is also what patch 0023 relies on).
# Pass an already patched FFmpeg tree for offline use, otherwise fetch the
# pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0080 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi
source_dir="$(cd "$source_dir" && pwd)"

build="$workdir/build"
mkdir -p "$build"
(cd "$build" && "$source_dir/configure" --disable-everything --disable-programs --disable-doc \
    --disable-autodetect --disable-asm --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-swresample \
    --enable-protocol=http --enable-protocol=tcp >"$workdir/configure.log" 2>&1) \
    || { cat "$workdir/configure.log"; exit 1; }
make -C "$build" -j"$(getconf _NPROCESSORS_ONLN)" libavformat/libavformat.a libavcodec/libavcodec.a \
    libavutil/libavutil.a >"$workdir/make.log" 2>&1 || { tail -50 "$workdir/make.log"; exit 1; }

cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
    -I"$build" -I"$source_dir" -o "$workdir/test" "$root/scripts/test_http_persistent.c" \
    "$build/libavformat/libavformat.a" "$build/libavcodec/libavcodec.a" "$build/libavutil/libavutil.a" \
    -lm -lpthread
"$workdir/test"
