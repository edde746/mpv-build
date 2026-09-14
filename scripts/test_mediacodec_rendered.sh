#!/usr/bin/env bash
# Compile the production rendered-frame feedback ring from the final Android
# ffmpeg series: the codec-thread producer in mediacodecdec_common.c and the
# av_mediacodec_drain_rendered consumer in mediacodec.c. Pass an already
# patched ffmpeg tree for offline use; otherwise fetch and patch the pinned
# source in a temporary directory.
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
    match = re.search(r'^(?:static )?(?:int |const char \*)' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise SystemExit(f'MediaCodec source region not found: {name} in {path}')
    return match.group(0)

(work / 'mediacodec_rendered.inc').write_text(
    extract('mediacodecdec_common.c', 'mediacodec_dec_rendered_push') + '\n\n' +
    extract('mediacodec.c', 'av_mediacodec_drain_rendered') + '\n\n' +
    extract('mediacodec.c', 'av_mediacodec_rendered_source') + '\n')
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -pthread -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_rendered.c"
"$workdir/test"
