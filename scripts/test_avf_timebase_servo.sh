#!/usr/bin/env bash
# Simulation test for the vo_avfoundation timebase servo
# (patch/libmpv/0016-avfoundation-timebase-rate-servo.patch).
#
# The servo and playback-rate decision functions are reconstructed VERBATIM
# from the patch's post-image (the marked "avf sync servo core" region, which
# is deliberately freestanding C) and compiled into a harness that models the
# CMTimebase and mpv's audio-slaved frame schedule under realistic clock
# disturbances. See test_avf_timebase_servo.c for the scenarios and invariants.
#
# Constraint: the extraction reads ONLY patch 0016, so the marked region must
# stay wholly inside it as added lines. A later patch editing the region would
# silently desynchronize this test from the shipped code; move such changes
# into 0016 instead.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0016-avfoundation-timebase-rate-servo.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/servo_core.inc" <<'PY'
import sys

inside = False
found_end = False
output = []
for raw in open(sys.argv[1], encoding="utf-8").read().splitlines():
    if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
        continue
    if not raw.startswith(("+", " ")):
        continue
    line = raw[1:]
    if "--- avf sync servo core" in line:
        inside = True
    if inside:
        output.append(line)
    if inside and "--- end avf sync servo core" in line:
        found_end = True
        break

if not found_end or not any("avf_servo_rate" in line for line in output):
    print(f"FAIL: servo core region not found in {sys.argv[1]}", file=sys.stderr)
    sys.exit(1)
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_timebase_servo.c" -lm

"$workdir/test"
