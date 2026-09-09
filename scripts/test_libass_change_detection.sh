#!/usr/bin/env bash
# Build the pinned, patched libass fork for the host and check that a static
# frame rendered twice is reported unchanged the second time (the layout cache
# must not hand the next frame a copy with a fresh bitmap pointer). Pass an
# already-patched libass tree for offline use; otherwise fetch and patch the
# pinned source in a temporary directory. Needs the host's freetype, fribidi,
# harfbuzz and libunibreak via pkg-config, autotools, and a system font.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

for pkg in freetype2 fribidi harfbuzz libunibreak; do
    pkg-config --exists "$pkg" || { echo "FAIL: host $pkg not found via pkg-config" >&2; exit 1; }
done

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    python3 - "$root" "$source_dir" <<'PY'
import json
import subprocess
import sys
from pathlib import Path
root, source = map(Path, sys.argv[1:])
entry = json.loads((root / 'versions.json').read_text())['components']['libass']
pin = entry | (entry.get('overrides', {}).get('android') or {})
subprocess.run(['git', 'clone', '--quiet', '--depth', '1', '--branch', pin['ref'],
                pin['url'], str(source)], check=True)
head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
if head != pin['commit']:
    raise SystemExit(f"libass {pin['ref']} is {head}, expected {pin['commit']}")
subprocess.run([sys.executable, 'scripts/patches.py', 'apply', 'libass', 'android', str(source)],
               cwd=root, check=True)
PY
fi

build="$workdir/build"
mkdir -p "$build"
(
    cd "$source_dir"
    [[ -f configure ]] || ./autogen.sh >/dev/null 2>&1
    cd "$build"
    "$source_dir/configure" --disable-shared --enable-static --with-pic \
        --disable-require-system-font-provider --disable-fontconfig >"$build/configure.log" 2>&1 \
        || { cat "$build/configure.log" >&2; exit 1; }
    make -j"$(getconf _NPROCESSORS_ONLN)" >"$build/make.log" 2>&1 \
        || { tail -40 "$build/make.log" >&2; exit 1; }
)

link=()
case "$(uname -s)" in
    Darwin) link=(-liconv -framework CoreText -framework CoreFoundation) ;;
esac
cc -O1 -std=c11 -Wall -Wextra -Werror -I"$source_dir" \
    -o "$workdir/test" "$root/scripts/test_libass_change_detection.c" \
    "$build/libass/.libs/libass.a" \
    $(pkg-config --libs freetype2 fribidi harfbuzz libunibreak) \
    "${link[@]}" -lpthread -lm
"$workdir/test"
