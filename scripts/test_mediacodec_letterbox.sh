#!/usr/bin/env bash
# Compile the production OSD-plane letterbox fill from patch 0106 against a
# recording GL stub. Accept an already patched mpv tree for offline use,
# otherwise fetch the pinned Android source.
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
entry = json.loads((root / 'versions.json').read_text())['components']['mpv']
pin = entry | (entry.get('overrides', {}).get('android') or {})
subprocess.run(['git', 'clone', '--quiet', '--depth', '1', '--branch', pin['ref'],
                pin['url'], str(source)], check=True)
head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
if head != pin['commit']:
    raise SystemExit(f"mpv {pin['ref']} is {head}, expected {pin['commit']}")
subprocess.run([sys.executable, 'scripts/patches.py', 'apply', 'mpv', 'android', str(source)],
               cwd=root, check=True)
PY
fi

python3 - "$source_dir/video/out/vo_mediacodec.c" "$workdir/mediacodec_letterbox.inc" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text()
parts = []
for pattern in (r'^struct osd_slot \{.*?^\};',
                r'^static int osd_gl_bar\(.*?^\}',
                r'^static void osd_gl_fill_letterbox\(.*?^\}'):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise SystemExit(f'letterbox fill not found: {pattern}')
    parts.append(match.group(0))
Path(sys.argv[2]).write_text('\n\n'.join(parts) + '\n')
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_letterbox.c"
"$workdir/test"
