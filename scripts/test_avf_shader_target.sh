#!/usr/bin/env bash
# Regression test for the size vo_avfoundation renders --glsl-shaders at, and
# the part of the frame that fills it, which ships in
# patches/mpv/pool/0004-avfoundation-video-output.patch: the patch that creates
# vo_avfoundation.m and avf_shaders.[hm] and carries every change to them.
#
# avf_shader_target() is reconstructed VERBATIM from the patch's post-image
# (the marked "avf shader target" region, which is deliberately freestanding C)
# together with struct avf_shaders_target from avf_shaders.h. See
# test_avf_shader_target.c for the scenarios.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0004-avfoundation-video-output.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

python3 "$root/scripts/extract.py" post-image "$patch_file" "$workdir/post_image.txt"
python3 "$root/scripts/extract.py" type "$workdir/post_image.txt" "$workdir/shader_target.inc" \
    --name avf_shaders_target
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/shader_target.inc" --append \
    --start "avf shader target" --patch --added-only --require "avf_shader_target"
cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_shader_target.c" -lm

"$workdir/test"
