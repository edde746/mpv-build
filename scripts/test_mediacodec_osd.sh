#!/usr/bin/env bash
# Compile the shared OSD render-ahead pipeline's scheduler core and request
# dispatcher (video/out/osd_ahead.[ch], patch 0031, with 0214's repeat of an
# unchanged frame) VERBATIM from the final Android series. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
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

# The freestanding types and the scheduler core are marked regions; the
# request dispatcher is a set of individual definitions.
python3 "$E" region "$header" "$workdir/mediacodec_osd_core.inc" \
    --start "osd ahead types" --require "OSD_CADENCE_SAMPLES"
python3 "$E" region "$module" "$workdir/mediacodec_osd_core.inc" --append \
    --start "osd ahead scheduler core" --require "osd_prefetch_plan"

python3 "$E" symbol "$module" "$workdir/mediacodec_osd_request.inc" \
    --fn osd_find_release --fn osd_free_slot --fn osd_post_result \
    --fn osd_notify_posted --fn osd_repeat_slot --fn osd_mark_served --fn osd_settle --fn osd_service_request \
    --fn osd_ahead_file_request --fn osd_ahead_is_settled \
    --fn osd_ahead_invalidate --fn osd_ahead_publish_release \
    --fn osd_ahead_take_newest

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_mediacodec_osd.c" -lm

"$workdir/test"
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/request-test" "$root/scripts/test_mediacodec_osd_request.c" -lm
"$workdir/request-test"

# The id vo_mediacodec gives a frame's request while the OSD is static (patch
# 0153): made with the window's size, which is not asked for while a subtitle
# decoder or a delayed plane keeps every frame requested anyway.
python3 "$E" symbol "$source_dir/video/out/vo_mediacodec.c" "$workdir/mediacodec_osd_static.inc" \
    --fn osd_static_id
cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$workdir" \
    -o "$workdir/static-test" "$root/scripts/test_mediacodec_osd_static.c"
"$workdir/static-test"
