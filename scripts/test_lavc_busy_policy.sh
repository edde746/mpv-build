#!/usr/bin/env bash
# Compile mpv's lavc_process against scripted send/receive callbacks and drive
# the busy policy a MediaCodec decoder is run under: when a both-port EAGAIN
# gets a re-poll, a wakeup wait, or nothing. Pass an already-patched mpv tree
# for offline use; otherwise fetch and patch the pinned source.
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
E="$root/scripts/extract.py"
inc="$workdir/lavc_busy_policy.inc"
python3 "$E" type "$source_dir/filters/f_decoder_wrapper.h" "$inc" \
    --name lavc_busy_policy --name lavc_state
python3 "$E" symbol "$source_dir/filters/f_decoder_wrapper.c" "$inc" --append \
    --fn lavc_process --return void
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$workdir" \
    "$root/scripts/test_lavc_busy_policy.c" -o "$workdir/test"
"$workdir/test"
