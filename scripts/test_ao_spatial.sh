#!/usr/bin/env bash
# Unit test for the "spatial" audio output
# (patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch):
# test_ao_spatial.m includes the patched audio/out/ao_spatial.m and drives it
# against a real AVSampleBufferRenderSynchronizer and a fake sink,
# including the latch that makes a failed stream fall back to PCM. macOS only.
# Pass an already-patched mpv tree for offline use; otherwise fetch and patch
# the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi
# The driver reads one build switch; the test supplies the mpv functions it calls.
printf '#pragma once\n#define HAVE_AVFOUNDATION 1\n' > "$workdir/config.h"
xcrun --sdk macosx clang -x objective-c -std=c11 -Wall -Wno-unused-parameter \
    -I"$workdir" -I"$source_dir" -I"$source_dir/include" \
    "$root/scripts/test_ao_spatial.m" \
    -framework AVFoundation -framework CoreMedia -framework Foundation -framework QuartzCore \
    -o "$workdir/test"
"$workdir/test"
