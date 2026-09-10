#!/usr/bin/env bash
# Compile the scheduler core and request dispatcher VERBATIM from patch 0001,
# following test_mediacodec_timing.sh. Keep these functions in patch 0001:
# extracting its post-image cannot see edits in later patches.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0001-vo-mediacodec-timed-surface-with-osd-plane.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/mediacodec_osd_core.inc" "$workdir/mediacodec_osd_request.inc" "$workdir/mediacodec_osd_types.inc" <<'PY'
import sys

inside = False
found_start = False
found_end = False
output = []
postimage = []
for raw in open(sys.argv[1], encoding="utf-8").read().splitlines():
    if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
        continue
    if not raw.startswith(("+", " ")):
        continue
    line = raw[1:]
    postimage.append(line)
    if line == "// --- mediacodec osd scheduler core":
        if found_start:
            raise SystemExit("FAIL: duplicate OSD scheduler core region")
        found_start = True
        inside = True
    if inside:
        output.append(line)
    if line == "// --- end mediacodec osd scheduler core":
        if not inside:
            raise SystemExit("FAIL: unexpected OSD scheduler core end")
        found_end = True
        inside = False

if not found_start or not found_end:
    raise SystemExit(f"FAIL: OSD scheduler core region not found in {sys.argv[1]}")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")

def extract(prefix):
    starts = [i for i, line in enumerate(postimage)
              if line.startswith(prefix) and not line.rstrip().endswith(";")]
    if len(starts) != 1:
        raise SystemExit(f"FAIL: expected one {prefix} in {sys.argv[1]}")
    start = starts[0]
    end = postimage.index("}", start)
    return postimage[start:end + 1]

types = []
for name in ("OSD_SLOTS", "OSD_RELEASE_HISTORY"):
    definitions = [line for line in postimage if line.startswith(f"#define {name} ")]
    if len(definitions) != 1:
        raise SystemExit(f"FAIL: expected one definition of {name}")
    types.extend(definitions)
start = postimage.index("struct osd_request {")
types.extend(postimage[start:postimage.index("};", start) + 1])
open(sys.argv[4], "w", encoding="utf-8").write("\n".join(types) + "\n")

request = []
for prefix in ("static struct osd_release osd_find_release(",
               "static int osd_free_slot(", "static void osd_post_result(",
               "static void osd_service_request(", "static void osd_file_request(",
               "static void osd_invalidate_locked("):
    request.extend(extract(prefix))
open(sys.argv[3], "w", encoding="utf-8").write("\n".join(request) + "\n")
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_osd.c" -lm

"$workdir/test"
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/request-test" "$root/scripts/test_mediacodec_osd_request.c" -lm
"$workdir/request-test"
