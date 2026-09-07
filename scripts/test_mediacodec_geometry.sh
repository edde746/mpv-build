#!/usr/bin/env bash
# Extract the shipped geometry implementation verbatim from patch 0001.
# Keep the marked region wholly in that patch: later patches must not edit it.
# libass rendering is exercised when an existing pkg-config installation exposes
# libass; this script never installs dependencies.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0001-vo-mediacodec-timed-surface-with-osd-plane.patch"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 - "$patch_file" "$workdir/mediacodec_geometry_core.inc" <<'PY'
import sys

inside = False
found_end = False
output = []
for raw in open(sys.argv[1], encoding="utf-8").read().splitlines():
    if not raw.startswith("+") or raw.startswith("+++"):
        continue
    line = raw[1:]
    if line == "// --- mediacodec geometry core":
        inside = True
    if inside:
        output.append(line)
    if line == "// --- end mediacodec geometry core" and inside:
        found_end = True
        break
if not found_end:
    sys.exit(f"FAIL: production geometry region not found in {sys.argv[1]}")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(output) + "\n")
PY

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_geometry.c"
"$workdir/test"

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libass; then
    # pkg-config emits shell word lists, as in the fork's other native runners.
    cc -O2 -std=c11 -Wall -Wextra -Werror -DHAVE_LIBASS -I"$workdir" \
        $(pkg-config --cflags libass) \
        -o "$workdir/test-libass" "$root/scripts/test_mediacodec_geometry.c" \
        $(pkg-config --libs libass)
    "$workdir/test-libass"
else
    echo "SKIP: libass rendering (existing pkg-config libass installation unavailable)"
fi
