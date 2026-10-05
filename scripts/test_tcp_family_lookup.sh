#!/usr/bin/env bash
# Compile tcp.c's per-family hostname lookup and connection-race restart
# (patch 0029) against a scripted resolver and race, under ASan and UBSan.
# Pass an already patched FFmpeg tree for offline use, otherwise fetch the
# pinned Android source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved ffmpeg/android series (series.common first,
    # which is where the lookup patch lives).
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi

# Everything between the block's own #if and #endif: the lookup threads, the
# waits, the address copies and tcp_open_by_family() itself.
python3 "$root/scripts/extract.py" range "$source_dir/libavformat/tcp.c" "$workdir/tcp_family_lookup.inc" \
    --first '#define LOOKUP_DELAY_US' --last '#endif /* TCP_SPLIT_LOOKUP */'

cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -I"$workdir" -o "$workdir/test" "$root/scripts/test_tcp_family_lookup.c"
"$workdir/test"
