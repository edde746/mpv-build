#!/usr/bin/env bash
# Compile vo_avfoundation's libass-over-BGRA and BGRA-over-BGRA blends (the
# scalar references and the NEON kernels, patches 0004 and 0043) VERBATIM from
# the final Apple series and check that they agree bit for bit. aarch64 hosts only. Pass an
# already-patched mpv tree for offline use; otherwise fetch and patch the
# pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"

case "$(uname -m)" in
    arm64|aarch64) ;;
    *) echo "test_avf_osd_blend: skipped (needs an aarch64 host)"; exit 0 ;;
esac

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

src="$source_dir/video/out/vo_avfoundation.m"
inc="$workdir/avf_osd_blend.inc"
x="$root/scripts/extract.py"
python3 "$x" symbol "$src" "$inc" --fn blend_libass_bgra_scalar --return void
python3 "$x" symbol "$src" "$inc" --append --fn div65025_u32_neon --return "inline uint16x4_t"
python3 "$x" symbol "$src" "$inc" --append --fn blend_libass_channel_neon --return "inline uint8x8_t"
python3 "$x" symbol "$src" "$inc" --append --fn blend_opaque_channel_neon --return "inline uint8x8_t"
python3 "$x" symbol "$src" "$inc" --append --fn blend_libass_bgra_neon --return void
# The premultiplied BGRA blend (bitmap subtitles, the OSD): since patch 0048 a
# part is blended in pieces, clipped to the tiles that changed, so a pixel is
# the kernel's in one frame and the scalar tail's in the next.
python3 "$x" symbol "$src" "$inc" --append --fn blend_bgra_over_bgra_scalar --return void
python3 "$x" symbol "$src" "$inc" --append --fn div255_u16_neon --return "inline uint16x8_t"
python3 "$x" symbol "$src" "$inc" --append --fn blend_over_channel_neon --return "inline uint8x8_t"
python3 "$x" symbol "$src" "$inc" --append --fn blend_bgra_over_bgra_neon --return void

cc -O2 -std=c11 -Wall -Wextra -Werror -I"$workdir" \
    -o "$workdir/test" "$root/scripts/test_avf_osd_blend.c"
"$workdir/test"
