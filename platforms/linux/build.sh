#!/usr/bin/env bash
set -euo pipefail

# Linux libmpv build driver.
#
# Builds the bundled libmpv.so for the linux platform group: dav1d, ffmpeg,
# shaderc and libplacebo static into an install prefix (with nv-codec-headers'
# header-only ffnvcodec feeding ffmpeg), our patched libass static on top,
# then mpv as a shared libmpv linked against all of them.
# Every pin comes from the repo-root versions.json with `overrides.linux`
# folded in; a missing pin is fatal, never defaulted.
#
# The script is runnable bare on any Linux with the toolchain packages
# installed, but the published artifacts must always come out of the pinned
# container recorded in toolchain/linux.txt -- the glibc the build links
# against is the floor every user inherits. CI wraps this script via
# platforms/linux/container-build.sh; local runs may do the same when docker
# is available.
#
# SRCDIR, when set, names a directory that must not exist yet; the sources are
# fetched and built there instead of in a mktemp directory. Compilers record
# the source path (__FILE__, debug info), so a random one makes every rebuild
# of a published key differ from the asset already published under it;
# container-build.sh sets a fixed one.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
VERSIONS_MANIFEST="${VERSIONS_MANIFEST:-$REPO_ROOT/versions.json}"
# Where `patches/<component>/` lives. The test harness points this at an empty
# directory so the stubbed build plan never depends on the real patch pools.
PATCHES_ROOT="${PATCHES_ROOT:-$REPO_ROOT}"

# Every pin this driver reads, resolved in one manifest pass with
# overrides.linux folded in: one python3 invocation instead of one per scalar
# (30 spawns, each re-parsing versions.json for a single value), and one place
# that decides what a missing pin means. A table row is
# <variable> <component> <key> [optional]; `optional` marks a key whose
# absence means "not offered" (a mirror URL) rather than a broken manifest.
# Pinned values are never optional: a missing commit stays fatal.
#
# The arguments name the acquisition each step below is written for. Every
# step clones a pinned git ref; a pin whose kind changes (an override that
# swaps a tag for a release archive, say) needs its step rewritten with it --
# the check fails loudly at parse time instead of cloning a ref the manifest
# no longer describes.
resolve_pins() {
  local dump line
  dump="$(python3 - "$VERSIONS_MANIFEST" "$@" <<'PY'
import json
import sys

SPECS = """
FFMPEG_VERSION ffmpeg version
FFMPEG_URL ffmpeg url
FFMPEG_REF ffmpeg ref
FFMPEG_COMMIT ffmpeg commit
DAV1D_VERSION dav1d version
DAV1D_URL dav1d url
DAV1D_MIRROR dav1d mirror optional
DAV1D_REF dav1d ref
DAV1D_COMMIT dav1d commit
SHADERC_VERSION shaderc version
SHADERC_URL shaderc url
SHADERC_REF shaderc ref
SHADERC_COMMIT shaderc commit
LIBPLACEBO_VERSION libplacebo version
LIBPLACEBO_URL libplacebo url
LIBPLACEBO_MIRROR libplacebo mirror optional
LIBPLACEBO_REF libplacebo ref
LIBPLACEBO_COMMIT libplacebo commit
LIBASS_VERSION libass version
LIBASS_URL libass url
LIBASS_REF libass ref
LIBASS_COMMIT libass commit
MPV_VERSION mpv version
MPV_URL mpv url
MPV_REF mpv ref
MPV_COMMIT mpv commit
NV_CODEC_HEADERS_VERSION nv-codec-headers version
NV_CODEC_HEADERS_URL nv-codec-headers url
NV_CODEC_HEADERS_REF nv-codec-headers ref
NV_CODEC_HEADERS_COMMIT nv-codec-headers commit
""".strip().splitlines()

with open(sys.argv[1], encoding="utf-8") as source:
    components = json.load(source)["components"]


def component(name):
    if name not in components:
        raise SystemExit(f"versions.json: no component {name!r}")
    return components[name]


for expectation in sys.argv[2:]:
    name, expected = expectation.split(":", 1)
    entry = component(name)
    actual = (entry.get("overrides") or {}).get("linux", {}).get("kind", entry.get("kind"))
    if actual != expected:
        raise SystemExit(f"versions.json pins {name} as kind '{actual}',"
                         f" but this build acquires it as '{expected}'")

for spec in SPECS:
    variable, name, key, *flags = spec.split()
    entry = component(name)
    override = (entry.get("overrides") or {}).get("linux") or {}
    value = override.get(key, entry.get(key, ""))
    if not isinstance(value, str) or (not value and "optional" not in flags):
        raise SystemExit(f"invalid versions.json pin: {name}.{key}")
    print(f"{variable}={value}")
PY
)" || exit 1

  while read -r line; do
    printf -v "${line%%=*}" '%s' "${line#*=}"
  done <<< "$dump"
}

resolve_pins dav1d:git nv-codec-headers:git ffmpeg:git shaderc:git libplacebo:git libass:git mpv:git

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
  # This exists because code.videolan.org (dav1d, libplacebo) refused
  # connections for well over two minutes at a time across several CI runs and
  # took every build with it.
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

# The epilogue every step shares: back to the source root, where the next step
# resolves its own tree from, then the step name so a long log stays scannable.
finish_step() {
  cd "$srcdir"
  printf '\n==> %s done.\n\n' "$1"
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

  if [ -n "${SRCDIR:-}" ]; then
    # A plain mkdir refuses an existing directory, so cleanup below can only
    # ever remove what this run created.
    mkdir -- "$SRCDIR"
    srcdir="$SRCDIR"
  else
    srcdir="$(mktemp -d)"
  fi
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
  finish_step dav1d

  # ─── Step 2: nv-codec-headers (ffnvcodec, headers only) ────────────────────
  # NVIDIA hardware decoding (ffmpeg's nvdec hwaccel, mpv's cuda hwdec and its
  # GL interop) compiles against ffnvcodec: headers plus a pkg-config file,
  # nothing to link. Its loader dlopen()s libcuda.so.1 and libnvcuvid.so.1 at
  # runtime, so the bundle gains no link-time dependency and a machine without
  # the NVIDIA driver simply has no CUDA hwdec - `hwdec=auto` moves on. It
  # installs into the prefix ahead of ffmpeg, whose configure and mpv's meson
  # both resolve `ffnvcodec` through pkg-config.
  echo "==> Installing nv-codec-headers $NV_CODEC_HEADERS_VERSION..."
  checkout_verified_ref \
    "$NV_CODEC_HEADERS_URL" "$NV_CODEC_HEADERS_REF" "$NV_CODEC_HEADERS_COMMIT" \
    "$srcdir/nv-codec-headers-${NV_CODEC_HEADERS_VERSION}"
  apply_patch_series nv-codec-headers "$srcdir/nv-codec-headers-${NV_CODEC_HEADERS_VERSION}"
  cd "nv-codec-headers-${NV_CODEC_HEADERS_VERSION}"

  make PREFIX="$prefix" install
  finish_step nv-codec-headers

  # ─── Step 3: ffmpeg (static libraries) ─────────────────────────────────────
  # vaapi, ffnvcodec and nvdec are all autodetected, and each one missing still
  # configures, builds and plays - on the CPU. Enabling them explicitly turns a
  # missing header into a configure failure instead.
  #
  # --enable-debug=1 compiles FFmpeg's C with -g1 (nasm's objects get
  # `-g -F dwarf` at any level): function and inlined-call DIEs plus line
  # tables, exactly what a crash tracker reads to name and place an FFmpeg
  # frame in libmpv. The full -g of a bare --enable-debug only adds variable,
  # parameter and type DIEs, which nothing symbolicating a crash uses, at a
  # multiple of the size. Debug info never changes codegen, so the -O level
  # is FFmpeg's default as before, and --disable-stripping still keeps the
  # install from stripping anything: package.sh copies the DWARF into the
  # symbols zip, and the tarball's libmpv.so stays unstripped.
  echo "==> Building ffmpeg $FFMPEG_VERSION (static, decoder-only)..."
  checkout_verified_ref \
    "$FFMPEG_URL" "$FFMPEG_REF" "$FFMPEG_COMMIT" \
    "$srcdir/ffmpeg-${FFMPEG_VERSION}"
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
    --enable-filter=aformat,aresample,bwdif,decimate,fieldmatch,format,loudnorm,null,scale \
    --enable-gnutls \
    --enable-vaapi \
    --enable-ffnvcodec \
    --enable-nvdec \
    --enable-libdav1d \
    --disable-vdpau \
    --enable-debug=1 \
    --disable-stripping

  make -j"$jobs"
  make install
  finish_step ffmpeg

  # ─── Step 4: shaderc (static library) ───────────────────────────────────────
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
  finish_step shaderc

  # ─── Step 5: libplacebo (static library) ───────────────────────────────────
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
  finish_step libplacebo

  # ─── Step 6: libass (static library, our fork) ─────────────────────────────
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
  finish_step libass

  # ─── Step 7: mpv (shared libmpv) ───────────────────────────────────────────
  echo "==> Building mpv $MPV_VERSION (shared libmpv only)..."
  checkout_verified_ref \
    "$MPV_URL" "$MPV_REF" "$MPV_COMMIT" \
    "$srcdir/mpv-${MPV_VERSION}"
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
  #
  # cuda-hwaccel/cuda-interop are pinned for the same reason: they need
  # ffnvcodec and resolve to disabled without it. They give NVIDIA GPUs the
  # nvdec/nvdec-copy hwdecs, which libva cannot serve there without a
  # third-party VA driver, and the CUDA GL interop that takes nvdec zero-copy
  # through any GL context, EGL or not.
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
    -Dcuda-hwaccel=enabled \
    -Dcuda-interop=enabled \
    -Dalsa=enabled \
    -Dpulse=enabled \
    -Dpipewire=enabled \
    -Dvdpau=disabled \
    -Dwayland=enabled \
    -Dx11=disabled

  ninja -C build -j"$jobs"
  ninja -C build install
  finish_step mpv
  echo "==> libmpv build complete. Output in $prefix"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
