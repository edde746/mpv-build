#!/usr/bin/env bash
# Compile the marked, freestanding MediaCodec timing core VERBATIM from the
# patch's post-image, following test_avf_timebase_servo.sh. Keep changes to this
# region in patch 0001: extracting it here cannot see edits in later patches.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0001-vo-mediacodec-timed-surface-with-osd-plane.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/mediacodec_timing_core.inc" <<'PY'
import sys

inside = False
found_start = False
found_end = False
output = []
for raw in open(sys.argv[1], encoding="utf-8").read().splitlines():
    if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
        continue
    if not raw.startswith(("+", " ")):
        continue
    line = raw[1:]
    if line == "// --- mediacodec timing core":
        if found_start:
            raise SystemExit("FAIL: duplicate MediaCodec timing core region")
        found_start = True
        inside = True
    if inside:
        output.append(line)
    if line == "// --- end mediacodec timing core":
        if not inside:
            raise SystemExit("FAIL: unexpected MediaCodec timing core end")
        found_end = True
        inside = False

if not found_start or not found_end:
    raise SystemExit(f"FAIL: MediaCodec timing core region not found in {sys.argv[1]}")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_timing.c" -lm

"$workdir/test"
