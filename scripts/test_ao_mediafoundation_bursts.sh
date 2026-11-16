#!/usr/bin/env bash
# Compile the IEC 61937 parser of ao_mediafoundation.c (windows 0300, the
# region between "// --- bursts" and "// --- end bursts") on the host and feed
# it: synthetic E-AC-3 bursts split at every size down to single bytes, odd
# payload lengths, garbage and bursts of other data types in between, an
# allocation failure in the middle, and a real stream from FFmpeg's spdif muxer
# (the muxer mpv's ad_spdif uses), whose payloads must add up to the E-AC-3
# frames it was given. Needs ffmpeg with the eac3 encoder and the spdif muxer.
# Pass an already-patched mpv tree for offline use; otherwise fetch and patch
# the pinned source.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv windows "$source_dir")
fi
python3 "$root/scripts/extract.py" region "$source_dir/audio/out/ao_mediafoundation.c" \
    "$workdir/bursts.inc" --start bursts --require bursts_feed --require bursts_reset
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -I"$workdir" "$root/scripts/test_ao_mediafoundation_bursts.c" -o "$workdir/test"

# Six channels, 1536 samples a frame: one frame per burst, as from a film.
ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:duration=2:sample_rate=48000" \
    -af "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0" -c:a eac3 -b:a 640k -f eac3 "$workdir/frames.eac3"
ffmpeg -nostdin -hide_banner -loglevel error -i "$workdir/frames.eac3" -c copy -f spdif "$workdir/carrier.spdif"
"$workdir/test" "$workdir/carrier.spdif" "$workdir/frames.eac3"
