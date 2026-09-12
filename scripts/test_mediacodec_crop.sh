#!/usr/bin/env bash
# Compile the production MediaCodec video format parser. Accept an already
# patched FFmpeg tree for offline use, otherwise fetch the pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    python3 - "$root" "$source_dir" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

root, source = map(Path, sys.argv[1:])
entry = json.loads((root / 'versions.json').read_text())['components']['ffmpeg']
pin = entry | (entry.get('overrides', {}).get('android') or {})
subprocess.run(['git', 'clone', '--quiet', '--depth', '1', '--branch', pin['ref'],
                pin['url'], str(source)], check=True)
head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
if head != pin['commit']:
    raise SystemExit(f"ffmpeg {pin['ref']} is {head}, expected {pin['commit']}")
subprocess.run([sys.executable, 'scripts/patches.py', 'apply', 'ffmpeg', 'android', str(source)],
               cwd=root, check=True)
PY
fi

python3 - "$source_dir/libavcodec/mediacodecdec_common.c" "$workdir/mediacodec_crop.inc" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text()
start = source.index('#define AMEDIAFORMAT_GET_INT32')
match = re.search(r'^static int mediacodec_dec_parse_video_format\([^;]*?\)\n\{.*?^\}',
                  source[start:], re.M | re.S)
if not match:
    raise SystemExit('MediaCodec video format parser not found')
Path(sys.argv[2]).write_text(source[start:start + match.end()] + '\n')
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_crop.c"
"$workdir/test"
