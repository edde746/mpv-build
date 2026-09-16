#!/usr/bin/env bash
# Compile the marked, freestanding Dolby Vision packet filter VERBATIM from the
# ffmpeg patch's post-image, following test_mediacodec_timing.sh. Keep changes
# to this region in patch 0001: extracting it here cannot see edits in later
# patches. libavutil and libdovi are stubbed by the test.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/ffmpeg/pool/0001-mediacodec-dolby-vision.patch"
# The profile 7 to 8.1 conversion the filter calls; see patch 0021.
helper_file="$root/patches/ffmpeg/pool/0021-dolby-vision-convert-profile7-rpu-to-p81.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/mediacodec_dv_filter.inc" \
         "$helper_file" "$workdir/dovi_convert.inc" <<'PY'
import sys


def region(path, start_marker, end_marker):
    """The added lines of the marked region in a patch, verbatim."""
    inside = False
    found_start = found_end = False
    output = []
    for raw in open(path, encoding="utf-8").read().splitlines():
        if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
            continue
        if not raw.startswith(("+", " ")):
            continue
        line = raw[1:]
        if line == start_marker:
            if found_start:
                raise SystemExit(f"FAIL: duplicate {start_marker} in {path}")
            found_start = True
            inside = True
        if inside:
            output.append(line)
        if line == end_marker:
            if not inside:
                raise SystemExit(f"FAIL: unexpected {end_marker} in {path}")
            found_end = True
            inside = False
    if not found_start or not found_end:
        raise SystemExit(f"FAIL: region {start_marker} not found in {path}")
    return output


dovi = region(sys.argv[3], "// --- dovi p7 conversion core",
              "// --- end dovi p7 conversion core")
open(sys.argv[4], "w", encoding="utf-8").write("\n".join(dovi) + "\n")

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
    if line == "// --- mediacodec dv filter":
        if found_start:
            raise SystemExit("FAIL: duplicate MediaCodec DV filter region")
        found_start = True
        inside = True
    if inside:
        output.append(line)
    if line == "// --- end mediacodec dv filter":
        if not inside:
            raise SystemExit("FAIL: unexpected MediaCodec DV filter region end")
        found_end = True
        inside = False

if not found_start or not found_end:
    raise SystemExit(f"FAIL: MediaCodec DV filter region not found in {sys.argv[1]}")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")
PY

# The converter region includes libdovi's header; the test stubs the API, so
# satisfy the include with an empty stand-in rather than a real libdovi.
mkdir -p "$workdir/stubinc/libdovi"
: > "$workdir/stubinc/libdovi/rpu_parser.h"

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
    -I"$workdir" -I"$workdir/stubinc" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_dv_filter.c"

"$workdir/test"
