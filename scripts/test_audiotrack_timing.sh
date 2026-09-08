#!/usr/bin/env bash
# Compile the production AudioTrack clock and buffer-publication path. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the pinned
# source in a temporary directory. No Android device or JNI runtime is needed.
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

python3 - "$source_dir/audio/out/ao_audiotrack.c" "$workdir" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text()
work = Path(sys.argv[2])

def extract(pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise SystemExit(f'AudioTrack source region not found: {pattern}')
    return match.group(0)

regions = [extract(r'^struct priv \{.*?^\};')]
for name in ('AudioTrack_resetPlayheadSmoothing', 'AudioTrack_smoothPlayhead',
             'AudioTrack_getPlaybackHeadPosition', 'AudioTrack_getLatency'):
    regions.append(extract(r'^static [^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}'))
(work / 'audiotrack_clock.inc').write_text('\n\n'.join(regions) + '\n')
# Exercise the deadline actually handed to ao_read_data, not a test-side
# reimplementation of that arithmetic. Leave the write/watchdog loop on Android.
thread = extract(r'^static MP_THREAD_VOID ao_thread\([^;]*?\)\n\{.*?^\}')
start = thread.index('            int read_samples =')
end = thread.index('\n', thread.index('ao_read_data(', start))
(work / 'audiotrack_read.inc').write_text(thread[start:end] + '\n(void)samples;\n')
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_audiotrack_timing.c" -lm
"$workdir/test"
