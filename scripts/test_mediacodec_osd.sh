#!/usr/bin/env bash
# Compile the scheduler core and request dispatcher VERBATIM from patch 0001,
# following test_mediacodec_timing.sh. Keep these functions in patch 0001:
# extracting its post-image cannot see edits in later patches.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0001-vo-mediacodec-timed-surface-with-osd-plane.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

E="$root/scripts/extract.py"

# The scheduler core is a marked region of the patch; the request dispatcher and
# the types it uses are individual definitions in that same post-image.
python3 "$E" region "$patch_file" "$workdir/mediacodec_osd_core.inc" \
    --start "mediacodec osd scheduler core" --patch --require "OSD_CADENCE_SAMPLES"

python3 "$E" post-image "$patch_file" "$workdir/postimage.txt"
python3 "$E" define "$workdir/postimage.txt" "$workdir/mediacodec_osd_types.inc" \
    --name OSD_SLOTS --name OSD_RELEASE_HISTORY
python3 "$E" type "$workdir/postimage.txt" "$workdir/mediacodec_osd_types.inc" --append \
    --name osd_request

python3 "$E" symbol "$workdir/postimage.txt" "$workdir/mediacodec_osd_request.inc" \
    --fn osd_find_release --fn osd_free_slot --fn osd_post_result \
    --fn osd_service_request --fn osd_file_request --fn osd_invalidate_locked

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_osd.c" -lm

"$workdir/test"
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/request-test" "$root/scripts/test_mediacodec_osd_request.c" -lm
"$workdir/request-test"
