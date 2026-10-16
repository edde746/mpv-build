#!/usr/bin/env bash
# Build and run the host tests of the Android library's JNI glue
# (platforms/android/lib/src/test/cpp): the session lifecycle and surface
# handoff, the event thread's callback order around an error end-file, and the
# UTF-8 crossing. They compile the production glue against fakes of jni.h and
# android/log.h and against the real headers of the pinned trees - mpv's
# include/mpv/client.h and the patched FFmpeg's libavcodec/jni.h - so no
# Android toolchain or device is needed. Pass an already-patched mpv tree and
# ffmpeg tree for offline use; otherwise fetch and patch the pinned Android
# sources in a temporary directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mpv_dir="${1:-}"
ffmpeg_dir="${2:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The pin contract lives in scripts/patches.py: it resolves the
# override-aware pin, clones the ref, proves HEAD is the pinned commit and
# applies the resolved android series.
if [[ -z "$mpv_dir" ]]; then
    mpv_dir="$workdir/mpv"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$mpv_dir")
fi
if [[ -z "$ffmpeg_dir" ]]; then
    ffmpeg_dir="$workdir/ffmpeg"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$ffmpeg_dir")
fi

cmake -S "$root/platforms/android/lib/src/test/cpp" -B "$workdir/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMPV_INCLUDE_DIR="$mpv_dir/include" \
    -DFFMPEG_INCLUDE_DIR="$ffmpeg_dir"
cmake --build "$workdir/build" --parallel
(cd "$workdir/build" && ctest --output-on-failure --no-tests=error)
