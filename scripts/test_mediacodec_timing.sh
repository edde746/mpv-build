#!/usr/bin/env bash
# Compile production timing, admission and draw/flip from the final Android
# patch series. Pass an already-patched mpv tree for offline use; otherwise
# fetch and patch the pinned source in a temporary directory.
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

python3 - "$source_dir" "$workdir" <<'PY'
import sys
import pathlib
import re

source_dir, directory = map(pathlib.Path, sys.argv[1:])
source = (source_dir / "video/out/vo_mediacodec.c").read_text()
core = (source_dir / "video/out/vo.c").read_text()
start = "// --- mediacodec timing core"
end = "// --- end mediacodec timing core"
if source.count(start) != 1 or source.count(end) != 1:
    raise SystemExit("FAIL: expected exactly one production MediaCodec timing core")
timing = source[source.index(start):source.index(end) + len(end)]
(directory / "mediacodec_timing_core.inc").write_text(timing + "\n")
# Compile production preparation, draw and flip, not a parallel scheduling model.
# Only platform/codec/OSD side effects are replaced by the host harness.
functions = [
    "read_vsync_sample", "vsync_sample_is_fresh", "display_period",
    "update_queue_timing", "get_release_target", "prepare_osd",
    "submit_deadline", "prepare_frame", "draw_frame", "flip_page",
    "reset_video",
]
driver = source[source.index("#define VSYNC_SAMPLE_MAX_AGE_NS"):
                source.index("static int64_t read_vsync_sample")]
for name in functions:
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;{]*\{.*?^\}\n",
                      source, re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit(f"FAIL: production function {name} not found")
    driver += match.group(0) + "\n"
(directory / "mediacodec_timing_driver.inc").write_text(driver)
cadence = source[source.index("#define OSD_CADENCE_SAMPLES"):
                 source.index("struct osd_spec {")]
(directory / "mediacodec_timing_cadence.inc").write_text(cadence)
match = re.search(r"^static bool prepare_queued_frame\([^;{]*\{.*?^\}\n",
                  core, re.MULTILINE | re.DOTALL)
if not match:
    raise SystemExit("FAIL: production VO preparation admission not found")
(directory / "mediacodec_timing_admission.inc").write_text(match.group(0))
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_timing.c" -lm

"$workdir/test"
