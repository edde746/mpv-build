#!/usr/bin/env bash
set -euo pipefail

# Linux libmpv build driver.
#
# Builds the bundled libmpv.so for the linux platform group: dav1d, ffmpeg,
# shaderc and libplacebo static into an install prefix, our patched libass
# static on top, then mpv as a shared libmpv linked against all of them.
# Every pin comes from the repo-root versions.json with `overrides.linux`
# folded in; a missing pin is fatal, never defaulted.
#
# The script is runnable bare on any Linux with the toolchain packages
# installed, but the published artifacts must always come out of the pinned
# container recorded in toolchain/linux.txt -- the glibc the build links
# against is the floor every user inherits. CI wraps this script via
# platforms/linux/container-build.sh; local runs may do the same when docker
# is available.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
VERSIONS_MANIFEST="${VERSIONS_MANIFEST:-$REPO_ROOT/versions.json}"
# Where `patches/<component>/` lives. The test harness points this at an empty
# directory so the stubbed build plan never depends on the real patch pools.
PATCHES_ROOT="${PATCHES_ROOT:-$REPO_ROOT}"

pin_value() {
  python3 - "$VERSIONS_MANIFEST" "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    manifest = json.load(source)
component = manifest["components"][sys.argv[2]]
override = (component.get("overrides") or {}).get("linux") or {}
value = override.get(sys.argv[3], component.get(sys.argv[3]))
if not isinstance(value, str) or not value:
    raise SystemExit(f"invalid versions.json pin: {sys.argv[2]}.{sys.argv[3]}")
print(value)
PY
}

# For keys that are genuinely optional, where absence means "not offered"
# rather than a broken manifest. Pinned values never come through here: a
# missing checksum or commit has to stay fatal.
pin_optional() {
  python3 - "$VERSIONS_MANIFEST" "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    manifest = json.load(source)
component = manifest["components"][sys.argv[2]]
override = (component.get("overrides") or {}).get("linux") or {}
value = override.get(sys.argv[3], component.get(sys.argv[3], ""))
if not isinstance(value, str):
    raise SystemExit(f"invalid versions.json pin: {sys.argv[2]}.{sys.argv[3]}")
print(value)
PY
}

# Each acquisition step below is written for one kind. When a pin's kind
# changes (say mpv's linux override is retired and the shared git pin takes
# over), the step must be rewritten with it -- fail loudly instead of
# downloading a tag archive that no longer exists.
require_kind() {
  local component="$1" expected="$2" actual
  actual="$(pin_value "$component" kind)"
  if [ "$actual" != "$expected" ]; then
    echo "versions.json pins $component as kind '$actual'," \
      "but this build acquires it as '$expected'" >&2
    return 1
  fi
}

require_kind dav1d git
require_kind ffmpeg archive
require_kind shaderc git
require_kind libplacebo git
require_kind libass git
require_kind mpv archive

FFMPEG_VERSION="$(pin_value ffmpeg version)"
FFMPEG_URL="$(pin_value ffmpeg url)"
FFMPEG_SHA256="$(pin_value ffmpeg sha256)"
DAV1D_VERSION="$(pin_value dav1d version)"
DAV1D_URL="$(pin_value dav1d url)"
DAV1D_MIRROR="$(pin_optional dav1d mirror)"
DAV1D_REF="$(pin_value dav1d ref)"
DAV1D_COMMIT="$(pin_value dav1d commit)"
SHADERC_VERSION="$(pin_value shaderc version)"
SHADERC_URL="$(pin_value shaderc url)"
SHADERC_REF="$(pin_value shaderc ref)"
SHADERC_COMMIT="$(pin_value shaderc commit)"
LIBPLACEBO_VERSION="$(pin_value libplacebo version)"
LIBPLACEBO_URL="$(pin_value libplacebo url)"
LIBPLACEBO_MIRROR="$(pin_optional libplacebo mirror)"
LIBPLACEBO_REF="$(pin_value libplacebo ref)"
LIBPLACEBO_COMMIT="$(pin_value libplacebo commit)"
LIBASS_VERSION="$(pin_value libass version)"
LIBASS_URL="$(pin_value libass url)"
LIBASS_REF="$(pin_value libass ref)"
LIBASS_COMMIT="$(pin_value libass commit)"
MPV_VERSION="$(pin_value mpv version)"
MPV_URL="$(pin_value mpv url)"
MPV_SHA256="$(pin_value mpv sha256)"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

download_verified() {
  local url="$1"
  local expected_sha256="$2"
  local destination="$3"
  local temporary
  local actual_sha256

  if [[ ! "$expected_sha256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "Invalid SHA-256 pin for $url" >&2
    return 1
  fi

  mkdir -p "$(dirname "$destination")"
  temporary="$(mktemp "${destination}.tmp.XXXXXX")"
  # Retries cover the transfer only. A checksum mismatch below is never retried:
  # that is a tampered or moved artefact, not a flaky connection, and trying
  # again would only turn a loud failure into an intermittent one.
  if ! curl \
    --fail \
    --location \
    --silent \
    --show-error \
    --retry 3 \
    --retry-connrefused \
    --retry-delay 5 \
    --connect-timeout 30 \
    --proto '=https,file' \
    --tlsv1.2 \
    --output "$temporary" \
    "$url"; then
    rm -f "$temporary"
    return 1
  fi

  actual_sha256="$(sha256_file "$temporary")"
  if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "SHA-256 mismatch for $url" >&2
    echo "Expected: $expected_sha256" >&2
    echo "Actual:   $actual_sha256" >&2
    rm -f "$temporary" "$destination"
    return 1
  fi

  mv "$temporary" "$destination"
}

checkout_verified_ref() {
  local url="$1"
  local ref="$2"
  local expected_commit="$3"
  local destination="$4"
  local mirror="${5:-}"
  local actual_commit
  local source
  local attempt

  if [[ ! "$expected_commit" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Invalid Git commit pin for $url at $ref" >&2
    return 1
  fi

  # Retries and the mirror cover the transfer, never the verification. The commit
  # pin below is checked identically whichever source answered, so a mirror can
  # only supply the same tree or fail - it cannot substitute another one.
  #
  # This exists because code.videolan.org, the only source fetched over git,
  # refused connections for well over two minutes at a time across several CI
  # runs and took every build with it.
  rm -rf "$destination"
  for source in "$url" ${mirror:+"$mirror"}; do
    for attempt in 1 2 3; do
      if git clone --quiet --depth 1 --branch "$ref" --no-checkout \
        "$source" "$destination"; then
        break 2
      fi
      rm -rf "$destination"
      # No point pausing before giving up on this source.
      if [ "$attempt" -lt 3 ]; then sleep $((attempt * 5)); fi
    done
    echo "Could not clone $source at $ref after 3 attempts" >&2
  done
  if [ ! -d "$destination" ]; then
    echo "No source produced $ref for $url" >&2
    return 1
  fi

  actual_commit="$(git -C "$destination" rev-parse 'HEAD^{commit}')"
  if [ "$actual_commit" != "$expected_commit" ]; then
    echo "Git ref mismatch for $ref" >&2
    echo "Expected: $expected_commit" >&2
    echo "Actual:   $actual_commit" >&2
    rm -rf "$destination"
    return 1
  fi

  git -C "$destination" checkout --quiet --detach "$expected_commit"
}

# The component's resolved linux patch series (series.common then
# series.linux), applied in authoritative order to a freshly acquired tree.
# An empty series is a silent no-op, so every component goes through here and
# a future series file starts applying without touching this script. The
# resolved series is part of the artifact's content key, so patched sources
# can never masquerade as pristine ones.
apply_patch_series() {
  local component="$1" tree="$2"
  (cd "$PATCHES_ROOT" && python3 "$REPO_ROOT/scripts/patches.py" apply "$component" linux "$tree")
}

cleanup_srcdir=""

cleanup() {
  if [ -n "$cleanup_srcdir" ]; then
    rm -rf -- "$cleanup_srcdir"
  fi
}

main() {
  local prefix="${PREFIX:-$(pwd)/libmpv-prefix}"
  local jobs="${JOBS:-$(nproc)}"
  local srcdir

  mkdir -p "$prefix"
  prefix="$(realpath "$prefix")"
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib/$(uname -m)-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"

  srcdir="$(mktemp -d)"
  cleanup_srcdir="$srcdir"
  trap cleanup EXIT
  cd "$srcdir"

  echo "==> Sources in $srcdir"
  echo "==> Install prefix: $prefix"
  echo ""

  # ─── Step 1: dav1d (static library) ────────────────────────────────────────
  # The bundled ffmpeg has no AV1 software decoder: its native av1 decoder is
  # hardware-accelerated only, and no libaom/libdav1d is linked in. When hwdec
  # is unavailable or cannot serve the source (an AV1 file on a GPU without AV1
  # decode), AV1 has no path at all - every packet fails, video hits EOF and
  # the plane goes black while audio keeps playing. dav1d is the software
  # floor under AV1, exactly as libass is for subtitles. It must come before
  # ffmpeg, whose configure resolves --enable-libdav1d against dav1d's
  # pkg-config file.
  echo "==> Building dav1d $DAV1D_VERSION (static)..."
  checkout_verified_ref \
    "$DAV1D_URL" "$DAV1D_REF" "$DAV1D_COMMIT" \
    "$srcdir/dav1d-v${DAV1D_VERSION}" "$DAV1D_MIRROR"
  apply_patch_series dav1d "$srcdir/dav1d-v${DAV1D_VERSION}"
  cd "dav1d-v${DAV1D_VERSION}"

  meson setup build \
    --prefix="$prefix" \
    --default-library=static \
    -Denable_tools=false \
    -Denable_tests=false \
    -Denable_examples=false \
    -Denable_docs=false

  ninja -C build -j"$jobs"
  ninja -C build install
  cd "$srcdir"
  echo ""
  echo "==> dav1d done."
  echo ""

  # ─── Step 2: ffmpeg (static libraries) ─────────────────────────────────────
  echo "==> Building ffmpeg $FFMPEG_VERSION (static, decoder-only)..."
  download_verified "$FFMPEG_URL" "$FFMPEG_SHA256" "$srcdir/ffmpeg.tar.xz"
  tar -xJf "$srcdir/ffmpeg.tar.xz"
  apply_patch_series ffmpeg "$srcdir/ffmpeg-${FFMPEG_VERSION}"
  cd "ffmpeg-${FFMPEG_VERSION}"

  ./configure \
    --prefix="$prefix" \
    --enable-gpl \
    --enable-version3 \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-programs \
    --disable-doc \
    --disable-encoders \
    --disable-muxers \
    --enable-muxer=spdif \
    --disable-devices \
    --disable-bsfs \
    --enable-bsf=aac_adtstoasc,av1_metadata,extract_extradata,h264_metadata,h264_mp4toannexb,hevc_metadata,hevc_mp4toannexb,vp9_metadata \
    --disable-filters \
    --enable-filter=aformat,aresample,bwdif,format,loudnorm,null,scale \
    --enable-gnutls \
    --enable-vaapi \
    --enable-libdav1d \
    --disable-vdpau \
    --disable-debug \
    --disable-stripping

  make -j"$jobs"
  make install
  cd "$srcdir"
  echo ""
  echo "==> ffmpeg done."
  echo ""

  # ─── Step 3: shaderc (static library) ───────────────────────────────────────
  echo "==> Building shaderc $SHADERC_VERSION (static)..."
  checkout_verified_ref \
    "$SHADERC_URL" "$SHADERC_REF" "$SHADERC_COMMIT" \
    "$srcdir/shaderc-v${SHADERC_VERSION}"
  apply_patch_series shaderc "$srcdir/shaderc-v${SHADERC_VERSION}"
  cd "shaderc-v${SHADERC_VERSION}"
  ./utils/git-sync-deps

  cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSHADERC_SKIP_TESTS=ON \
    -DSHADERC_SKIP_EXAMPLES=ON \
    -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

  cmake --build build -j"$jobs"
  cmake --install build
  cd "$srcdir"
  echo ""
  echo "==> shaderc done."
  echo ""

  # ─── Step 4: libplacebo (static library) ───────────────────────────────────
  echo "==> Building libplacebo $LIBPLACEBO_VERSION (static)..."
  checkout_verified_ref \
    "$LIBPLACEBO_URL" "$LIBPLACEBO_REF" "$LIBPLACEBO_COMMIT" \
    "$srcdir/libplacebo-v${LIBPLACEBO_VERSION}" "$LIBPLACEBO_MIRROR"
  apply_patch_series libplacebo "$srcdir/libplacebo-v${LIBPLACEBO_VERSION}"
  cd "libplacebo-v${LIBPLACEBO_VERSION}"
  git submodule update --init --recursive

  meson setup build \
    --prefix="$prefix" \
    --default-library=static \
    -Dvulkan=disabled \
    -Dd3d11=disabled \
    -Ddemos=false \
    -Dtests=false

  ninja -C build -j"$jobs"
  ninja -C build install
  cd "$srcdir"
  echo ""
  echo "==> libplacebo done."
  echo ""

  # ─── Step 5: libass (static library, our fork) ─────────────────────────────
  # mpv would happily take the distro's libass, but the whole point of the
  # fork pin is that every platform renders subtitles with the same patched
  # libass. Built static for the same reason dav1d is: a shared fork of a
  # library the host also ships would have to travel in the bundle and shadow
  # the system one, which is exactly the coupling the rest of the plan avoids.
  # freetype, fribidi, harfbuzz and fontconfig stay host (container) deps:
  # the pinned toolchain container carries versions above libass 0.18.3's
  # minima, they are unpatched upstream libraries, and mpv links them from the
  # host either way (see toolchain/linux.txt).
  echo "==> Building libass $LIBASS_VERSION (static, patched fork)..."
  checkout_verified_ref \
    "$LIBASS_URL" "$LIBASS_REF" "$LIBASS_COMMIT" \
    "$srcdir/libass-${LIBASS_VERSION}"
  apply_patch_series libass "$srcdir/libass-${LIBASS_VERSION}"
  cd "libass-${LIBASS_VERSION}"

  ./autogen.sh
  # --enable-fontconfig rather than the autodetected default, for the same
  # reason mpv's features are pinned below: without fontconfig libass still
  # builds, mpv still links it, and system fonts silently stop resolving.
  ./configure \
    --prefix="$prefix" \
    --enable-static \
    --disable-shared \
    --with-pic \
    --enable-fontconfig

  make -j"$jobs"
  make install
  cd "$srcdir"
  echo ""
  echo "==> libass done."
  echo ""

  # ─── Step 6: mpv (shared libmpv) ───────────────────────────────────────────
  echo "==> Building mpv $MPV_VERSION (shared libmpv only)..."
  download_verified "$MPV_URL" "$MPV_SHA256" "$srcdir/mpv.tar.gz"
  tar -xzf "$srcdir/mpv.tar.gz"
  apply_patch_series mpv "$srcdir/mpv-${MPV_VERSION}"
  cd "mpv-${MPV_VERSION}"

  # The runner's only video path is a Wayland subsurface, and it hands mpv
  # MPV_RENDER_PARAM_WL_DISPLAY so VAAPI can find the device instead of falling
  # back to software decoding. A libmpv built without Wayland cannot use that.
  # VDPAU goes with X11 - it has no Wayland backend at all.
  #
  # drm/vaapi-drm/egl are pinned enabled, not left on auto: mpv's `drm` feature
  # silently drops to disabled when libdisplay-info is missing, and every VAAPI
  # path that does not depend on a display server - vaapi-copy's standalone
  # render-node device and the GL dmabuf interop - is derived from it. Shipping
  # that build quietly lands every source on software decoding (the 2.13.0
  # Fedora report). Enabled means the configure fails when the pieces are
  # absent instead of degrading in silence.
  meson setup build \
    --prefix="$prefix" \
    -Dlibmpv=true \
    -Dcplayer=false \
    -Dbuild-date=false \
    -Dlua=enabled \
    -Djavascript=enabled \
    -Dcplugins=disabled \
    -Dmanpage-build=disabled \
    -Djack=disabled \
    -Dvulkan=disabled \
    -Dd3d11=disabled \
    -Dgl=enabled \
    -Degl=enabled \
    -Ddrm=enabled \
    -Dvaapi=enabled \
    -Dvaapi-drm=enabled \
    -Dvaapi-wayland=enabled \
    -Dalsa=enabled \
    -Dpulse=enabled \
    -Dpipewire=enabled \
    -Dvdpau=disabled \
    -Dwayland=enabled \
    -Dx11=disabled

  ninja -C build -j"$jobs"
  ninja -C build install
  echo ""
  echo "==> mpv done."
  echo ""
  echo "==> libmpv build complete. Output in $prefix"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
