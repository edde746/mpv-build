#!/usr/bin/env bash
# Regression test for the frame-space OSD geometry used by the PiP compositing
# path (patch/libmpv/0024-avfoundation-frame-space-osd-in-pip.patch).
#
# The geometry constructor and the subtitle bounding-box mapping are
# reconstructed VERBATIM from the patch's post-image (the marked "avf
# frame-space osd core" region, which is deliberately freestanding C) and
# compiled into a harness that models the letterboxed window-space geometry they
# replace, libass line placement, and the composite path vo_avfoundation then
# takes. See test_avf_pip_frame_space_osd.c for the scenarios and invariants.
#
# Constraint: the extraction reads ONLY patch 0024, so the marked region must
# stay wholly inside it as added lines. A later patch editing the region would
# silently desynchronize this test from the shipped code; move such changes into
# 0024 instead.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0024-avfoundation-frame-space-osd-in-pip.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/frame_space_core.inc" <<'PY'
import sys

inside = False
found_end = False
output = []
for raw in open(sys.argv[1], encoding="utf-8").read().splitlines():
    if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
        continue
    if not raw.startswith("+"):
        continue
    line = raw[1:]
    if "--- avf frame-space osd core" in line:
        inside = True
    if inside:
        output.append(line)
    if inside and "--- end avf frame-space osd core" in line:
        found_end = True
        break

required = ("avf_frame_space_geometry", "avf_map_subtitle_rect")
if not found_end or not all(any(name in line for line in output) for name in required):
    print(f"FAIL: frame-space osd core region not found in {sys.argv[1]}", file=sys.stderr)
    sys.exit(1)
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_pip_frame_space_osd.c"

"$workdir/test"
