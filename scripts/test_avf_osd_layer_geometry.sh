#!/usr/bin/env bash
# Regression test for where vo_avfoundation places its inline OSD layer, which
# ships in patches/mpv/pool/0004-avfoundation-video-output.patch: the patch
# that creates vo_avfoundation.m and carries every change to it.
#
# layer_untransformed_frame() is extracted VERBATIM from the patch's post-image
# and compiled against real Core Animation layers, so this needs macOS. See
# test_avf_osd_layer_geometry.m for the scenarios and invariants.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0004-avfoundation-video-output.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 "$root/scripts/extract.py" post-image "$patch_file" "$workdir/post_image.m"
python3 "$root/scripts/extract.py" symbol "$workdir/post_image.m" "$workdir/osd_layer_geometry.inc" \
    --fn layer_untransformed_frame --return CGRect

cc -O2 -fobjc-arc -Wall -Wextra -Werror -I"$workdir" \
    -framework Foundation -framework QuartzCore -framework AVFoundation \
    -o "$workdir/test" "$root/scripts/test_avf_osd_layer_geometry.m"

"$workdir/test"
