#!/usr/bin/env bash
# Exercise OSD retirement from the final Android patch series. Pass an already-
# patched mpv tree for offline use; otherwise fetch and patch the pinned source.
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
python3 - "$source_dir/video/out/vo_mediacodec.c" "$workdir" <<'PY'
import pathlib
import re
import sys
source, work = map(pathlib.Path, sys.argv[1:])
text = source.read_text()
functions = []
for name in ('osd_init', 'osd_uninit', 'update_opts'):
    match = re.search(r'^static void ' + name + r'\([^;{]*\{.*?^\}\n', text, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit(f'Production function {name} not found')
    functions.append(match.group(0))
(work / 'mediacodec_rebind.inc').write_text('\n'.join(functions))
PY
cc -std=c11 -Wall -Wextra -Werror -pthread -I"$workdir" \
    "$root/scripts/test_mediacodec_rebind.c" -o "$workdir/test"
"$workdir/test"
