#!/usr/bin/env bash
# Drive vo_mediacodec's process-lifetime Choreographer vsync sampler, sliced
# from the final Android patch series, against a model of Android 11's
# AChoreographer. Pass an already-patched mpv tree for offline use; otherwise
# fetch and patch the pinned source in a temporary directory.
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

src="$source_dir/video/out/vo_mediacodec.c"
E="$root/scripts/extract.py"

# The libandroid entry points the sampler resolves, as it declares them.
python3 "$E" range "$src" "$workdir/mediacodec_vsync_types.inc" \
    --first "typedef AChoreographer *(*mp_get_choreographer_fn)(void);" \
    --last "// --- mediacodec geometry core"

# The sampler itself, whole: its state, every callback, its thread, and the
# attach/detach the VO calls; then the one request the VO thread makes.
python3 "$E" symbol "$src" "$workdir/mediacodec_vsync.inc" --fn restore_frame_time
python3 "$E" range "$src" "$workdir/mediacodec_vsync.inc" --append \
    --first "// The Choreographer is per thread, and AOSP never frees it" \
    --last "static AVBufferRef *create_mediacodec_device_ref"
python3 "$E" symbol "$src" "$workdir/mediacodec_vsync.inc" --append \
    --fn request_vsync_sample

# Production mpv code is not written for -Wunused-parameter.
cc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -pthread \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_mediacodec_vsync.c"

"$workdir/test"
