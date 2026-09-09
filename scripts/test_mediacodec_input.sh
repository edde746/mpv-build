#!/usr/bin/env bash
# Compile the production MediaCodec input sizing and packet submission path.
# Pass an already-patched ffmpeg tree for offline use; otherwise fetch and patch
# the pinned source in a temporary directory. No Android device or JNI runtime
# is needed: the MediaCodec wrapper is stubbed by the test.
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

python3 - "$source_dir/libavcodec" "$workdir" <<'PY'
import re
import sys
from pathlib import Path

lavc, work = map(Path, sys.argv[1:])

def extract(path, name):
    source = (lavc / path).read_text()
    match = re.search(r'^(?:static )?int ' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise SystemExit(f'MediaCodec source region not found: {name} in {path}')
    return match.group(0)

(work / 'mediacodec_input.inc').write_text(
    extract('mediacodecdec.c', 'video_max_input_size') + '\n\n' +
    extract('mediacodecdec_common.c', 'ff_mediacodec_dec_send') + '\n')
PY

# FFmpeg builds its own sources without -Wsign-compare.
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_input.c"
"$workdir/test"
