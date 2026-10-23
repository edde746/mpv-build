#!/usr/bin/env bash
# Compile the FFmpeg log routing of the patched common/av_log.c (patch 0209):
# its instance list, init_libav(), uninit_libav(), mp_av_log_set_thread() and
# the av_log callback, and log FFmpeg lines from threads of two mpv instances
# alive at once: each line must reach the instance whose thread logged it,
# before and after the other instance is gone (an embedder's next player that
# never saw the "HTTP error 404" FFmpeg logged for it). Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # 0209 sits in series.common, so any group carries it.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv android "$source_dir")
fi

E="$root/scripts/extract.py"
av_log="$source_dir/common/av_log.c"
inc="$workdir/av_log_routing.inc"
# The instance struct and the routing state, up to the first function.
python3 "$E" range "$av_log" "$inc" --first "struct av_log_instance {" \
    --last "static int av_log_level_to_mp_level"
python3 "$E" symbol "$av_log" "$inc" --append --fn av_log_level_to_mp_level --return int
python3 "$E" symbol "$av_log" "$inc" --append --fn get_av_log
python3 "$E" symbol "$av_log" "$inc" --append --fn avclass_item_name
python3 "$E" symbol "$av_log" "$inc" --append --fn thread_log_instance
python3 "$E" symbol "$av_log" "$inc" --append --fn mp_msg_av_log_callback --return void
python3 "$E" symbol "$av_log" "$inc" --append --fn init_libav --return void
python3 "$E" symbol "$av_log" "$inc" --append --fn uninit_libav --return void
python3 "$E" symbol "$av_log" "$inc" --append --fn mp_av_log_set_thread --return void

cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -pthread -I"$workdir" \
    "$root/scripts/test_av_log_routing.c" -o "$workdir/test"
"$workdir/test"
