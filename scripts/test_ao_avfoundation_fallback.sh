#!/usr/bin/env bash
# Behavioral test for the PCM fallback of ao_avfoundation's compressed
# (AVPlayer) sink (patches/mpv/pool/0040-ao-decode-the-playing-track-to-pcm-when-passthrough-fails.patch
# and 0041-avfoundation-say-when-avplayer-never-asked-for-the-stream.patch)
# and its clock anchor (0042-avfoundation-anchor-the-passthrough-clock-to-the-playhead.patch):
# test_ao_avfoundation_fallback.m includes the patched audio/out/ao_avfoundation.m
# and drives it with a real AVPlayer in a process that never services the main
# dispatch queue, so AVPlayer never asks for the stream and the startup
# supervisor falls back. Its deadlines run on a fake clock. macOS only.
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
# The build switches of a macOS build; the test supplies the mpv functions the
# driver calls.
printf '#pragma once\n#define HAVE_AVFOUNDATION 1\n#define HAVE_COREAUDIO 1\n#define HAVE_MACOS_12_FEATURES 1\n' \
    > "$workdir/config.h"
xcrun --sdk macosx clang -x objective-c -std=c11 -fno-objc-arc -Wall -Wno-unused-parameter \
    -I"$workdir" -I"$source_dir" -I"$source_dir/include" \
    "$root/scripts/test_ao_avfoundation_fallback.m" \
    -framework AVFoundation -framework CoreMedia -framework Foundation \
    -o "$workdir/test"
"$workdir/test"
