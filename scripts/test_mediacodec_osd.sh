#!/usr/bin/env bash
# Compile the shared OSD render-ahead pipeline's scheduler core and request
# dispatcher (video/out/osd_ahead.[ch], patch 0031) and vo_mediacodec's swap
# lead VERBATIM from the final Android series. Pass an already-patched mpv
# tree for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/android series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

E="$root/scripts/extract.py"
header="$source_dir/video/out/osd_ahead.h"
module="$source_dir/video/out/osd_ahead.c"
vo="$source_dir/video/out/vo_mediacodec.c"

# The freestanding types, the scheduler core and the swap lead are marked
# regions; the request dispatcher is a set of individual definitions.
python3 "$E" region "$header" "$workdir/mediacodec_osd_core.inc" \
    --start "osd ahead types" --require "OSD_CADENCE_SAMPLES"
python3 "$E" region "$module" "$workdir/mediacodec_osd_core.inc" --append \
    --start "osd ahead scheduler core" --require "osd_prefetch_plan"
python3 "$E" region "$vo" "$workdir/mediacodec_osd_core.inc" --append \
    --start "mediacodec osd swap lead" --require "osd_swap_lead_observe"

python3 "$E" symbol "$module" "$workdir/mediacodec_osd_request.inc" \
    --fn osd_find_release --fn osd_free_slot --fn osd_post_result \
    --fn osd_service_request --fn osd_ahead_file_request \
    --fn osd_ahead_invalidate --fn osd_ahead_publish_release

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_osd.c" -lm

"$workdir/test"
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/request-test" "$root/scripts/test_mediacodec_osd_request.c" -lm
"$workdir/request-test"
