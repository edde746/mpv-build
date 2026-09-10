#!/usr/bin/env bash
# Compile the production AudioTrack clock, write, and reset paths. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the pinned
# source in a temporary directory. JNI is injected; no Android device is needed.
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

regions = [extract(r'^enum \{\n    RAW_SYNC_PA.*?^\};'),
           extract(r'^struct priv \{.*?^\};')]
regions += re.findall(r'^#define (?:IEC61937_|STALL_)[^\n]+', source, re.M)
for name in ('AudioTrack_resetPlayheadSmoothing', 'AudioTrack_resetClock',
             'AudioTrack_Recreate', 'AudioTrack_smoothPlayhead',
             'AudioTrack_getPlaybackHeadPosition', 'AudioTrack_getLatency',
             'AudioTrack_unwrapIEC61937', 'AudioTrack_write',
             'AudioTrack_beginRecovery', 'AudioTrack_recreateOrFail',
             'ao_thread', 'monitor_thread', 'stop', 'start'):
    regions.append(extract(r'^static [^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}'))
(work / 'audiotrack_write.inc').write_text('\n\n'.join(regions) + '\n')
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_audiotrack_timing.c" -lm
"$workdir/test"

cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -I"$workdir" \
    -o "$workdir/test_write" "$root/scripts/test_audiotrack_write.c" -lm
"$workdir/test_write"
