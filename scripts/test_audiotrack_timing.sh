#!/usr/bin/env bash
# Compile the production AudioTrack clock, write, and reset paths. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the pinned
# source in a temporary directory. JNI is injected; no Android device is needed.
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

src="$source_dir/audio/out/ao_audiotrack.c"
E="$root/scripts/extract.py"

# The clock: the raw-sync enum, the private state, and the four functions that
# read it. Exercise the deadline actually handed to ao_read_data, not a test-side
# reimplementation of that arithmetic; the write/watchdog loop stays on Android.
python3 "$E" type "$src" "$workdir/audiotrack_clock.inc" --name priv
python3 "$E" symbol "$src" "$workdir/audiotrack_clock.inc" --append \
    --fn AudioTrack_resetPlayheadSmoothing --fn AudioTrack_smoothPlayhead \
    --fn AudioTrack_getPlaybackHeadPosition --fn AudioTrack_getLatency
python3 "$E" range "$src" "$workdir/audiotrack_read.inc" \
    --first "            int read_samples =" --last "ao_read_data(" --last-inclusive
printf '(void)samples;\n' >> "$workdir/audiotrack_read.inc"

# The write path: the IEC61937 carriers, the stall constants, then every
# production function it drives.
python3 "$E" type "$src" "$workdir/audiotrack_write.inc" --member RAW_SYNC_PA
python3 "$E" type "$src" "$workdir/audiotrack_write.inc" --append --name priv
python3 "$E" define "$src" "$workdir/audiotrack_write.inc" --append \
    --name IEC61937_AC3 --name IEC61937_DTS1 --name IEC61937_DTS2 \
    --name IEC61937_DTS3 --name IEC61937_EAC3 \
    --name STALL_TIMEOUT_NS --name STALL_RECOVERED_NS --name STALL_MAX_RECREATES --name STALL_POLL_NS
python3 "$E" symbol "$src" "$workdir/audiotrack_write.inc" --append \
    --fn AudioTrack_resetPlayheadSmoothing --fn AudioTrack_resetClock \
    --fn AudioTrack_Recreate --fn AudioTrack_smoothPlayhead \
    --fn AudioTrack_getPlaybackHeadPosition --fn AudioTrack_getLatency \
    --fn AudioTrack_unwrapIEC61937 --fn AudioTrack_write \
    --fn AudioTrack_beginRecovery --fn AudioTrack_recreateOrFail \
    --fn ao_thread --fn monitor_thread --fn stop --fn start

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_audiotrack_timing.c" -lm
"$workdir/test"

cc -O2 -std=c11 -Wall -Wextra -Werror -pthread -I"$workdir" \
    -o "$workdir/test_write" "$root/scripts/test_audiotrack_write.c" -lm
"$workdir/test_write"
