#!/usr/bin/env bash
# Exercise OSD retirement from the final Android patch series. Pass an already-
# patched mpv tree for offline use; otherwise fetch and patch the pinned source.
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
# The render thread's lifetime is the shared pipeline's (osd_ahead.c); the
# presenter's and the window's are the VO's.
python3 "$root/scripts/extract.py" symbol "$source_dir/video/out/osd_ahead.c" \
    "$workdir/mediacodec_rebind.inc" --return bool --fn osd_ahead_start
python3 "$root/scripts/extract.py" symbol "$source_dir/video/out/osd_ahead.c" \
    "$workdir/mediacodec_rebind.inc" --append --return void \
    --fn osd_ahead_stop --fn osd_ahead_destroy
python3 "$root/scripts/extract.py" symbol "$source_dir/video/out/vo_mediacodec.c" \
    "$workdir/mediacodec_rebind.inc" --append --return void \
    --fn osd_init --fn osd_uninit --fn update_opts
cc -std=c11 -Wall -Wextra -Werror -pthread -I"$workdir" \
    "$root/scripts/test_mediacodec_rebind.c" -o "$workdir/test"
"$workdir/test"
