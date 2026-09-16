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

# The geometry core ships as added lines of patch 0001.
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/mediacodec_geometry_core.inc" \
    --start "mediacodec geometry core" --patch --added-only --require "mediacodec_osd_geometry"

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
