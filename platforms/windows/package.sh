#!/usr/bin/env bash
# Zip the mpv-dev tree a build.sh run assembled under its content-addressed
# asset name.
#
# usage: platforms/windows/package.sh <x86_64|aarch64> [--key <key>] [--out <dir>]
#
# The archive layout matches the sourceforge mpv-dev 7z packages plezy already
# consumes: libmpv-2.dll, libmpv.dll.a and include/mpv/*.h at the archive ROOT
# (no wrapping directory), so the plezy windows/CMakeLists.txt swap is a
# URL + hash change only -- FetchContent and the raw-extraction ARM64 path
# both keep working unchanged.
#
# --key defaults to `scripts/keys.py keys --platform-group windows`; pass it
# explicitly to package before/without group support in a checkout.
set -euo pipefail

usage() {
  echo "usage: platforms/windows/package.sh <x86_64|aarch64> [--key <key>] [--out <dir>]" >&2
  exit 2
}

ARCH="${1:-}"
case "$ARCH" in
  x86_64 | aarch64) ;;
  *) usage ;;
esac
shift

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
KEY=""
OUT="$ROOT/dist/release"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --key)
      KEY="${2:?--key needs a value}"
      shift 2
      ;;
    --out)
      OUT="${2:?--out needs a value}"
      shift 2
      ;;
    *) usage ;;
  esac
done

BUILD="$ROOT/build/windows/$ARCH"
shopt -s nullglob
DEV_DIRS=("$BUILD"/mpv-dev-*/)
shopt -u nullglob
if [[ "${#DEV_DIRS[@]}" -ne 1 ]]; then
  echo "error: expected exactly one mpv-dev-* tree in $BUILD, found ${#DEV_DIRS[@]}; run platforms/windows/build.sh $ARCH first" >&2
  exit 1
fi
DEV_DIR="${DEV_DIRS[0]%/}"

if [[ -z "$KEY" ]]; then
  KEY="$(python3 "$ROOT/scripts/keys.py" keys --platform-group windows |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["libmpv-windows"])')"
fi

mkdir -p "$OUT"
ASSET="$OUT/libmpv-windows-$KEY-$ARCH.zip"
rm -f "$ASSET"
# Sorted input and -X (no extra fs attributes) keep the archive as stable as
# zip can make it; immutability is guaranteed by the key in the name, not by
# byte-reproducibility.
(cd "$DEV_DIR" && find . -type f | LC_ALL=C sort | zip -q -X -9 "$ASSET" -@)
echo "$ASSET"
