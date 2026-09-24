#!/usr/bin/env bash
# Rebuild raw TrueHD access units from real IEC 61937 MAT bursts with the
# production reassembly (AudioTrack_unwrapIEC61937 and the AudioTrack_thd*
# helpers), and compare them byte for byte with the units FFmpeg packed.
# The streams come from the host FFmpeg's own TrueHD encoder and spdif muxer
# at test time, so no media is committed:
#   48 kHz sine        small units in wide padding
#   192 kHz noise      units larger than the 2560-byte MAT slot, so the MAT
#                      codes split units and units run on across frames
#   44.1 kHz sine      a rate family the 192 kHz raw track cannot clock
# Pass an already-patched mpv tree for offline use; otherwise fetch and patch
# the pinned source in a temporary directory. A second argument names an extra
# TrueHD stream to check (a Blu-ray remux, say); it is never committed. Needs a
# host ffmpeg with the truehd encoder and the spdif muxer.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
extra="${2:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi
command -v ffmpeg >/dev/null || { echo "error: needs a host ffmpeg" >&2; exit 1; }

src="$source_dir/audio/out/ao_audiotrack.c"
E="$root/scripts/extract.py"
inc="$workdir/audiotrack_truehd.inc"
python3 "$E" type "$src" "$inc" --member RAW_SYNC_PA
python3 "$E" type "$src" "$inc" --append --name priv
python3 "$E" define "$src" "$inc" --append \
    --name IEC61937_AC3 --name IEC61937_DTS1 --name IEC61937_DTS2 \
    --name IEC61937_DTS3 --name IEC61937_DTSHD --name IEC61937_EAC3 \
    --name IEC61937_TRUEHD --name MAT_FRAME_SIZE --name MAT_BURST_SIZE \
    --name MAT_START_LEN --name MAT_MIDDLE_POS --name MAT_MIDDLE_LEN --name MAT_END_POS \
    --name THD_UNITS_PER_S --name THD_MAX_UNIT --name THD_GROUP_UNITS \
    --name DTSHD_BURST_HEADER
python3 "$E" symbol "$src" "$inc" --append \
    --fn thd_raw_failed --fn AudioTrack_thdDemote --fn AudioTrack_thdReset \
    --fn AudioTrack_thdLoseSync --fn AudioTrack_thdPut --fn AudioTrack_thdStreamWord \
    --fn AudioTrack_thdMatWord --fn AudioTrack_thdGroup --fn AudioTrack_thdTake \
    --fn AudioTrack_unwrapIEC61937

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_audiotrack_truehd.c"

encode() { # name lavfi-source channels
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$2" -ac "$3" \
        -c:a truehd -strict experimental -f truehd "$workdir/$1.thd"
    ffmpeg -hide_banner -loglevel error -y -i "$workdir/$1.thd" -c copy \
        -f spdif "$workdir/$1.spdif"
}
encode s48 "sine=f=440:r=48000:d=4" 6
encode n192 "anoisesrc=r=192000:d=3:a=0.5" 6
encode s44 "sine=f=440:r=44100:d=2" 2

"$workdir/test" stream "$workdir/s48.thd" "$workdir/s48.spdif"
"$workdir/test" stream "$workdir/n192.thd" "$workdir/n192.spdif"
"$workdir/test" demote "$workdir/s44.thd" "$workdir/s44.spdif"
if [[ -n "$extra" ]]; then
    ffmpeg -hide_banner -loglevel error -y -i "$extra" -map 0:a:0 -c copy \
        -f truehd "$workdir/extra.thd"
    ffmpeg -hide_banner -loglevel error -y -i "$workdir/extra.thd" -c copy \
        -f spdif "$workdir/extra.spdif"
    "$workdir/test" stream "$workdir/extra.thd" "$workdir/extra.spdif"
fi
