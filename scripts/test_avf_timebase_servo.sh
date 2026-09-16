#!/usr/bin/env bash
# Simulation test for the vo_avfoundation timebase servo
# (patch/libmpv/0016-avfoundation-timebase-rate-servo.patch).
#
# The servo and playback-rate decision functions are reconstructed VERBATIM
# from the patch's post-image (the marked "avf sync servo core" region, which
# is deliberately freestanding C) and compiled into a harness that models the
# CMTimebase and mpv's audio-slaved frame schedule under realistic clock
# disturbances. See test_avf_timebase_servo.c for the scenarios and invariants.
#
# Constraint: the extraction reads ONLY patch 0016, so the marked region must
# stay wholly inside it as added lines. A later patch editing the region would
# silently desynchronize this test from the shipped code; move such changes
# into 0016 instead.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0016-avfoundation-timebase-rate-servo.patch"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# The servo core ships as one marked region of patch 0016, with its context
# lines (the region sits inside existing upstream code).
python3 "$root/scripts/extract.py" region "$patch_file" "$workdir/servo_core.inc" \
    --start "avf sync servo core" --patch --require "avf_servo_rate"

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_timebase_servo.c" -lm

"$workdir/test"
