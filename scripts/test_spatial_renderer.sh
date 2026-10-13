#!/usr/bin/env bash
# Usage: test_spatial_renderer.sh <patched-mpv> <built-spatial-dir> <iec-file> <1=truehd|2=dtshd> <expect-failure:0|1>
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$1"
library_dir="$2"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
xcrun clang -fobjc-arc -I"$source_dir" -c "$root/scripts/test_spatial_renderer.m" -o "$workdir/test.o"
xcrun swiftc "$workdir/test.o" "$library_dir/libmpv_spatial.a" "$library_dir/libspatial_audio_core.a" \
    -framework AVFoundation -framework Foundation -framework CoreMedia -framework AudioToolbox \
    -framework Accelerate -o "$workdir/test"
"$workdir/test" "$4" "$3" "$5"
