#!/usr/bin/env bash
# Production feeder + pinned unpacker, synthetic IEC input, no media or audio.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/scripts/patches.py" fetch-pinned mpv apple "$work/mpv"
python3 "$root/scripts/patches.py" fetch-pinned spatialaudiobridge apple "$work/sab"
xcrun --sdk macosx swiftc -swift-version 6 -parse-as-library \
    -target "$(uname -m)-apple-macos26.0" \
    "$work/mpv/audio/out/ao_spatial.swift" \
    "$work/sab/Sources/SpatialAudioBridge/IEC61937Unpacker.swift" \
    "$root/scripts/test_spatial_feed.swift" \
    -framework AVFoundation -framework CoreMedia -o "$work/test"
"$work/test"
