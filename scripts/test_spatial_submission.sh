#!/usr/bin/env bash
# Compile the complete patched renderer with synthetic codec/output doubles.
# CoreMedia creates sample buffers, but no audio renderer or device is opened.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/scripts/patches.py" fetch-pinned spatialaudiobridge apple "$work/sab"
python3 - "$root" "$work/host-and-checks.swift" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
patch = (root / 'patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch').read_text()
section = patch.split('+++ b/audio/out/ao_spatial.swift\n', 1)[1].split('diff --git ', 1)[0]
source = ''.join(line[1:] for line in section.splitlines(True) if line.startswith('+'))
clock = source.split('// Submission and rate changes', 1)[1].split('// Compiled with SpatialAudioBridge', 1)[0]
checks = (root / 'scripts/test_spatial_submission.swift').read_text()
Path(sys.argv[2]).write_text('import AVFoundation\nimport Synchronization\n// Submission and rate changes' + clock + checks)
PY
xcrun --sdk macosx swiftc -swift-version 6 -warnings-as-errors -parse-as-library \
    -target "$(uname -m)-apple-macos26.0" \
    "$work/sab/Sources/SpatialAudioBridge/SpatialAudioRenderer.swift" \
    "$work/host-and-checks.swift" \
    -framework AVFoundation -framework CoreMedia -framework Accelerate -o "$work/test"
"$work/test"
