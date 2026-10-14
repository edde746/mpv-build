#!/usr/bin/env bash
# Simulation test for the vo_avfoundation timebase servo, which ships in
# patches/mpv/pool/0004-avfoundation-video-output.patch: the patch that
# creates vo_avfoundation.m and carries every change to it.
#
# The servo and playback-rate decision functions are reconstructed VERBATIM
# from the patch's post-image (the marked "avf sync servo core" region, which
# is deliberately freestanding C) and compiled into a harness that models the
# CMTimebase and mpv's audio-slaved frame schedule under realistic clock
# disturbances. See test_avf_timebase_servo.c for the scenarios and invariants.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0004-avfoundation-video-output.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The servo core ships as one marked region of the file the patch creates.
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/servo_core.inc" \
    --start "avf sync servo core" --patch --added-only --require "avf_servo_rate"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_timebase_servo.c" -lm

"$workdir/test"
