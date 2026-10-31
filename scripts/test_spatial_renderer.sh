#!/usr/bin/env bash
# Play IEC 61937 TrueHD or DTS-HD through the real, muted spatial renderer on
# macOS (see docs/apple.md). Make the IEC file with FFmpeg's spdif muxer
# (-dtshd_rate 768000 for DTS-HD); the optional libdovi archive is force-loaded
# to check the prelinked Rust core coexists with it.
# Usage: test_spatial_renderer.sh <patched-mpv> <built-spatial-dir> <iec-file> <1=truehd|2=dtshd> <expect-failure:0|1> [libdovi-archive]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$1"
library_dir="$2"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
link_flags=(-framework Accelerate)
if [[ -n "${6:-}" ]]; then
    # Pull in libdovi's entire Rust runtime to expose symbol collisions.
    link_flags+=(-Xlinker -force_load -Xlinker "$6")
fi
xcrun clang -fobjc-arc -I"$source_dir" -c "$root/scripts/test_spatial_renderer.m" -o "$workdir/test.o"
xcrun swiftc "$workdir/test.o" "$library_dir/libmpv_spatial.a" "$library_dir/libspatial_audio_core.a" \
    -framework AVFoundation -framework Foundation -framework CoreMedia -framework AudioToolbox \
    "${link_flags[@]}" -o "$workdir/test"
"$workdir/test" "$4" "$3" "$5"
