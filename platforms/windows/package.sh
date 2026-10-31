#!/usr/bin/env bash
# Zip the mpv-dev tree a build.sh run assembled under its content-addressed
# asset name, and libmpv-2.dll's PDB into the unkeyed symbols zip
# libmpv-windows-<arch>-symbols.zip beside it, which
# `scripts/keys.py record-platform` names by the same key.
#
# usage: platforms/windows/package.sh <x86_64|aarch64>
#
# The archive layout matches the sourceforge mpv-dev 7z packages plezy already
# consumes: libmpv-2.dll, libmpv.dll.a and include/mpv/*.h at the archive ROOT
# (no wrapping directory), so the plezy windows/CMakeLists.txt swap is a
# URL + hash change only -- FetchContent and the raw-extraction ARM64 path
# both keep working unchanged. Beside them, licenses/<component>/ holds the
# licence texts of every component linked into libmpv-2.dll, copied from the
# source trees the build compiled. The name's content key comes from
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

# libmpv-2.pdb is the PDB lld wrote when it linked libmpv-2.dll; the
# copy-binary step pin_packages.py extends saves it into mpv-debug/, which
# upstream moves to mpv-debug-<cpu>-<date>-git-<sha>/ beside the dev tree.
shopt -s nullglob
DEBUG_DIRS=("$BUILD"/mpv-debug-*/)
shopt -u nullglob
if [[ "${#DEBUG_DIRS[@]}" -ne 1 ]]; then
  echo "error: expected exactly one mpv-debug-* tree in $BUILD, found ${#DEBUG_DIRS[@]}; run platforms/windows/build.sh $ARCH first" >&2
  exit 1
fi
PDB="${DEBUG_DIRS[0]%/}/libmpv-2.pdb"
if [[ ! -f "$PDB" ]]; then
  echo "error: $PDB is missing; the libmpv-2.pdb capture in pin_packages.py did not run" >&2
  exit 1
fi

KEY="$(python3 "$ROOT/scripts/keys.py" keys --platform-group windows |
  python3 -c 'import json,sys; print(json.load(sys.stdin)["libmpv-windows"])')"

mkdir -p "$OUT"
# Debug symbols first, so a DLL whose PDB does not match never leaves an
# asset behind: symbols.py fails unless the PDB's GUID and age are the ones
# libmpv-2.dll's CodeView record names, and writes the zip reproducibly
# (sorted, -X, 2020-01-01 mtimes, mode 0644). lld derives that GUID from a
# hash of the whole PDB, so a reproduced DLL implies a reproduced PDB.
SYMBOLS="$OUT/libmpv-windows-$ARCH-symbols.zip"
rm -f "$SYMBOLS"
python3 "$ROOT/scripts/symbols.py" pack "$SYMBOLS" --pair "$DEV_DIR/libmpv-2.dll" "$PDB"

# Licence texts: pin_packages.py names the components libmpv-2.dll links
# (mpv's package graph in this build tree, plus the llvm and mingw-w64
# runtime the toolchain links in) and the source tree each was built from;
# licenses.py copies their texts and fails on a component without one, and
# check fails unless every keyed component has its folder.
LICENCE_SOURCES="$(python3 "$HERE/pin_packages.py" --print-licence-sources "$BUILD")"
PAIRS=()
while IFS= read -r pair; do
  PAIRS+=("$pair")
done <<< "$LICENCE_SOURCES"
rm -rf "$DEV_DIR/licenses"
python3 "$ROOT/scripts/licenses.py" collect --group windows "$DEV_DIR/licenses" "${PAIRS[@]}"
# --group goes first: the argparse of ubuntu-24.04's Python 3.12.3 gives
# `check`'s component list nothing when an option follows the directory.
python3 "$ROOT/scripts/licenses.py" check --group windows "$DEV_DIR/licenses" "${PAIRS[@]%%=*}"

ASSET="$OUT/libmpv-windows-$KEY-$ARCH.zip"
rm -f "$ASSET"
# Reproducible bytes: the release skips re-uploading an existing
# content-addressed name, so a rebuild of the same key MUST produce the
# identical archive or the recorded checksum drifts from the published bytes.
# zip stores each file's DOS mtime and mode, so normalize both (the mode the
# apple zips use: the tree's modes come from the umask the build ran under);
# sorted input and -X (no extra fs attributes) cover the rest.
(cd "$DEV_DIR" && find . -type f -exec touch -t 202001010000 {} + \
  && find . -type f -exec chmod u+rw,go-w,a+rX {} + \
  && find . -type f | LC_ALL=C sort | zip -q -X -9 "$ASSET" -@)
echo "$ASSET"
echo "$SYMBOLS"
