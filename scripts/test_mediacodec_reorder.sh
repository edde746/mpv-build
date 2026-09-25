#!/usr/bin/env bash
# Compile the production MediaCodec presentation-order recovery
# (libavcodec/mediacodec_reorder.c) for the host and drive it with a model
# decoder. Pass an already-patched ffmpeg tree for offline use; otherwise fetch
# and patch the pinned source in a temporary directory. The module needs no
# configured tree: libavutil/avutil.h is stubbed down to AV_NOPTS_VALUE.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned ffmpeg android "$source_dir")
fi

mkdir -p "$workdir/include/libavutil"
cat > "$workdir/include/libavutil/avutil.h" <<'EOF'
#include <stdint.h>
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
EOF

cc -O2 -std=c11 -Wall -Wextra -Werror -Wno-sign-compare \
    -I"$workdir/include" -I"$source_dir/libavcodec" \
    -o "$workdir/test" \
    "$root/scripts/test_mediacodec_reorder.c" "$source_dir/libavcodec/mediacodec_reorder.c"
"$workdir/test"
