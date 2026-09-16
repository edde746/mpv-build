#!/usr/bin/env bash
# Zip the mpv-dev tree a build.sh run assembled under its content-addressed
# asset name.
#
# usage: platforms/windows/package.sh <x86_64|aarch64>
#
# The archive layout matches the sourceforge mpv-dev 7z packages plezy already
# consumes: libmpv-2.dll, libmpv.dll.a and include/mpv/*.h at the archive ROOT
# (no wrapping directory), so the plezy windows/CMakeLists.txt swap is a
# URL + hash change only -- FetchContent and the raw-extraction ARM64 path
# both keep working unchanged. The name's content key comes from
# `scripts/keys.py keys --platform-group windows`.
set -euo pipefail

usage() {
  echo "usage: platforms/windows/package.sh <x86_64|aarch64>" >&2
  exit 2
}

ARCH="${1:-}"
case "$ARCH" in
  x86_64 | aarch64) ;;
  *) usage ;;
esac
[[ $# -le 1 ]] || usage

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/dist/release"

BUILD="$ROOT/build/windows/$ARCH"
shopt -s nullglob
DEV_DIRS=("$BUILD"/mpv-dev-*/)
shopt -u nullglob
if [[ "${#DEV_DIRS[@]}" -ne 1 ]]; then
  echo "error: expected exactly one mpv-dev-* tree in $BUILD, found ${#DEV_DIRS[@]}; run platforms/windows/build.sh $ARCH first" >&2
  exit 1
fi
DEV_DIR="${DEV_DIRS[0]%/}"

KEY="$(python3 "$ROOT/scripts/keys.py" keys --platform-group windows |
  python3 -c 'import json,sys; print(json.load(sys.stdin)["libmpv-windows"])')"

mkdir -p "$OUT"
ASSET="$OUT/libmpv-windows-$KEY-$ARCH.zip"
rm -f "$ASSET"
# Reproducible bytes: the release skips re-uploading an existing
# content-addressed name, so a rebuild of the same key MUST produce the
# identical archive or the recorded checksum drifts from the published bytes.
# zip stores each file's DOS mtime, so normalize them; sorted input and -X
# (no extra fs attributes) cover the rest.
(cd "$DEV_DIR" && find . -type f -exec touch -t 202001010000 {} + \
  && find . -type f | LC_ALL=C sort | zip -q -X -9 "$ASSET" -@)
echo "$ASSET"
