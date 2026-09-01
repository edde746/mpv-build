#!/usr/bin/env bash
set -euo pipefail

# Packages the install prefix build.sh produced into one content-addressed
# tarball per architecture:
#
#     libmpv-linux-<key>-<x86_64|aarch64>.tar.zst
#
# The artifact serves two roles at once, so it carries two kinds of content:
#
#   runtime bundle source -- exactly the set plezy's CI copied out of
#     libmpv-prefix into the Flutter bundle: the versioned libmpv.so* chain
#     (symlinks preserved) and libshaderc_shared.so*, which shaderc's install
#     always emits and libmpv links even in the static-libs build.
#
#   compile-time pkg-config target -- the include/ tree and every pkgconfig/
#     directory in the prefix. All .pc files travel, not just mpv.pc:
#     pkg-config resolves mpv.pc's Requires.private even for --cflags, so a
#     tree without dav1d.pc and friends fails `pkg-config --cflags mpv`.
#     The static .a archives themselves are deliberately left behind; they
#     are already absorbed into libmpv.so. Every occurrence of the build
#     prefix in the staged .pc files is rewritten to a ${pcfiledir}-relative
#     root, so the unpacked tree works from any directory with nothing but
#     PKG_CONFIG_PATH. `pkg-config --define-prefix` is NOT the answer here:
#     it guesses the prefix as the .pc file's grandparent, which is wrong for
#     the multiarch lib/<triplet>/pkgconfig layout meson uses for mpv.pc, and
#     it cannot fix the literal prefix paths shaderc's cmake bakes into
#     Cflags outside ${prefix}.
#
# Environment:
#   PREFIX   install prefix to package     (default: ./libmpv-prefix)
#   KEY      content key for the name      (required; from scripts/keys.py)
#   OUT_DIR  where the tarball lands       (default: ./dist/release)
#   ARCH     architecture tag in the name  (default: uname -m)

PREFIX="${PREFIX:-$(pwd)/libmpv-prefix}"
OUT_DIR="${OUT_DIR:-$(pwd)/dist/release}"
ARCH="${ARCH:-$(uname -m)}"

if [ -z "${KEY:-}" ]; then
  echo "KEY is required: the content key naming this artifact" \
    "(scripts/keys.py keys --platform-group linux)" >&2
  exit 1
fi
if [ ! -d "$PREFIX" ]; then
  echo "PREFIX $PREFIX does not exist; run platforms/linux/build.sh first" >&2
  exit 1
fi
case "$ARCH" in
  x86_64 | aarch64) ;;
  *)
    echo "unexpected architecture '$ARCH'; the linux group publishes x86_64 and aarch64" >&2
    exit 1
    ;;
esac

PREFIX="$(realpath "$PREFIX")"

stage="$(mktemp -d)"
trap 'rm -rf -- "$stage"' EXIT

# Same discovery plezy's CI used: meson installs libmpv under lib/ or the
# multiarch lib/<triplet>/ depending on the distro layout, so it is found,
# never assumed. libshaderc_shared is a plain cmake install and stays lib/.
libmpv_so="$(find "$PREFIX" -name 'libmpv.so' | head -1)"
if [ -z "$libmpv_so" ]; then
  echo "no libmpv.so under $PREFIX; the build did not complete" >&2
  exit 1
fi
libmpv_dir="$(dirname "$libmpv_so")"
if ! compgen -G "$PREFIX/lib/libshaderc_shared.so*" >/dev/null; then
  echo "no libshaderc_shared.so* under $PREFIX/lib; the build did not complete" >&2
  exit 1
fi

relative() {
  local path="$1"
  printf '%s\n' "${path#"$PREFIX"/}"
}

libmpv_rel="$(relative "$libmpv_dir")"
mkdir -p "$stage/$libmpv_rel" "$stage/lib"
cp -a "$libmpv_dir"/libmpv.so* "$stage/$libmpv_rel/"
cp -a "$PREFIX"/lib/libshaderc_shared.so* "$stage/lib/"

if [ ! -d "$PREFIX/include" ]; then
  echo "no include/ under $PREFIX; the artifact could not be compiled against" >&2
  exit 1
fi
cp -a "$PREFIX/include" "$stage/include"

found_pkgconfig=0
for pkgconfig in "$PREFIX"/lib/pkgconfig "$PREFIX"/lib/*-linux-gnu/pkgconfig; do
  [ -d "$pkgconfig" ] || continue
  found_pkgconfig=1
  destination="$stage/$(relative "$pkgconfig")"
  mkdir -p "$(dirname "$destination")"
  cp -a "$pkgconfig" "$destination"
  # ${pcfiledir} is the directory holding the .pc file; walk up to the staged
  # root: lib/pkgconfig is two levels deep, lib/<triplet>/pkgconfig is three.
  case "$(relative "$pkgconfig")" in
    lib/pkgconfig) pcroot='${pcfiledir}/../..' ;;
    *) pcroot='${pcfiledir}/../../..' ;;
  esac
  for pc in "$destination"/*.pc; do
    [ -f "$pc" ] || continue
    rewritten="$(mktemp)"
    sed "s|$PREFIX|$pcroot|g" "$pc" >"$rewritten"
    mv "$rewritten" "$pc"
  done
done
if [ "$found_pkgconfig" -eq 0 ]; then
  echo "no pkgconfig directory under $PREFIX; the artifact could not be compiled against" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
artifact="$OUT_DIR/libmpv-linux-${KEY}-${ARCH}.tar.zst"
tar --zstd -C "$stage" -cf "$artifact" .
echo "==> Packaged $artifact"
