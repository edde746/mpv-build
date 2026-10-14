#!/usr/bin/env bash
set -euo pipefail

# No-network verification harness for platforms/linux/build.sh, ported from
# plezy's linux/packaging/build-libmpv_test.sh and adapted to versions.json:
# pins come from a stub manifest in the components/overrides.linux shape, and
# the override fold itself is under test -- the base entries below carry
# deliberately wrong values, so the run only succeeds if overrides.linux wins.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=build.sh
source "$SCRIPT_DIR/build.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

assert_absent() {
  [ ! -e "$1" ] || fail "unexpected path remains: $1"
}

init_repository() {
  mkdir -p "$1"
  git -C "$1" init --quiet
  git -C "$1" config user.name "mpv-build provenance test"
  git -C "$1" config user.email "provenance-test@invalid.example"
}

temporary="$(mktemp -d)"
trap 'rm -rf "$temporary"' EXIT

repository="$temporary/repository"
checkout="$temporary/checkout"
init_repository "$repository"
printf 'first\n' >"$repository/input.txt"
git -C "$repository" add input.txt
git -C "$repository" commit --quiet -m first
git -C "$repository" -c tag.gpgsign=false tag release
approved_commit="$(git -C "$repository" rev-parse HEAD)"
checkout_verified_ref "file://$repository" release "$approved_commit" "$checkout"
[ "$(git -C "$checkout" rev-parse HEAD)" = "$approved_commit" ] ||
  fail "verified checkout selected the wrong commit"

# A dead primary must fall through to the mirror, because the whole point is
# that one unreachable host cannot stop the build. This runs while the tag still
# points at the approved commit; the moved-tag case is below.
unreachable="file://$temporary/definitely-not-a-repository"
checkout_verified_ref "$unreachable" release "$approved_commit" "$checkout" "file://$repository"
[ "$(git -C "$checkout" rev-parse HEAD)" = "$approved_commit" ] ||
  fail "mirror fallback selected the wrong commit"

# And the mirror answers to the same pin. A mirror serving a different tree is
# the one thing a fallback must never quietly accept.
mirror_repository="$temporary/mirror"
init_repository "$mirror_repository"
printf 'substituted\n' >"$mirror_repository/input.txt"
git -C "$mirror_repository" add input.txt
git -C "$mirror_repository" commit --quiet -m substituted
git -C "$mirror_repository" -c tag.gpgsign=false tag release
if checkout_verified_ref "$unreachable" release "$approved_commit" "$checkout" "file://$mirror_repository"; then
  fail "mirror serving another commit was accepted"
fi
assert_absent "$checkout"

printf 'second\n' >"$repository/input.txt"
git -C "$repository" commit --quiet -am second
git -C "$repository" -c tag.gpgsign=false tag --force release >/dev/null
if checkout_verified_ref "file://$repository" release "$approved_commit" "$checkout"; then
  fail "moved tag was accepted"
fi
assert_absent "$checkout"

# ─── The build plan ─────────────────────────────────────────────────────────
# Everything above checks how sources arrive. What they are then configured
# with decides whether the feature works at all, and it fails quietly: a libmpv
# built without -Dwayland=enabled still compiles, still links and still plays,
# it just has no Wayland backend, so the render context cannot accept
# MPV_RENDER_PARAM_WL_DISPLAY, vaapi finds no device and hardware decoding
# drops to a copy-back path. Nothing crashes, so nothing else notices. Run
# main() for real against stub build tools and assert the argument vectors they
# were handed.

stub_bin="$temporary/stub-bin"
stub_extra="$temporary/stub-extra"
records="$temporary/records"
mkdir -p "$stub_bin" "$stub_extra" "$records" "$temporary/tmp"

# Deliberately unlike the pinned versions: every path below is derived from the
# manifest, so a stub that matched by accident would prove nothing.
ffmpeg_version="9.9.9"
dav1d_version="2.2.2"
shaderc_version="6.6.6"
libplacebo_version="7.7.7"
libass_version="4.4.4"
mpv_version="8.8.8"
nv_codec_headers_version="5.5.5"

# Each stub records the vector it was called with, one argument per line, plus
# the directory it ran in - which is what tells mpv's meson call apart from
# libplacebo's. An argument containing a newline would corrupt the record, and
# none of these ever does: they are literal flags and mktemp -d paths.
make_stub() {
  local directory="$1" name="$2" extra="${3:-}"
  cat >"$directory/$name" <<STUB
#!/usr/bin/env bash
set -euo pipefail
record="\$(mktemp "$records/record.XXXXXX")"
{ printf '%s\n' "$name" "\$(pwd)"; [ \$# -eq 0 ] || printf '%s\n' "\$@"; } >"\$record"
$extra
STUB
  chmod +x "$directory/$name"
}

# Every source arrives through checkout_verified_ref; ffmpeg's configure and
# libass's autogen.sh come out of their stub repositories, exactly where the
# real build finds them.
make_stub "$stub_extra" configure
for tool in make cmake meson ninja; do
  make_stub "$stub_bin" "$tool"
done

# ffmpeg runs ./configure out of its own checkout, so the recording configure
# stub is committed into its stub repository.
ffmpeg_repository="$temporary/ffmpeg-source"
init_repository "$ffmpeg_repository"
cp -- "$stub_extra/configure" "$ffmpeg_repository/configure"
git -C "$ffmpeg_repository" add configure
git -C "$ffmpeg_repository" update-index --chmod=+x configure
git -C "$ffmpeg_repository" commit --quiet -m ffmpeg
git -C "$ffmpeg_repository" -c tag.gpgsign=false tag release
ffmpeg_commit="$(git -C "$ffmpeg_repository" rev-parse HEAD)"

mpv_repository="$temporary/mpv-source"
init_repository "$mpv_repository"
printf 'stub mpv\n' >"$mpv_repository/meson.build"
git -C "$mpv_repository" add meson.build
git -C "$mpv_repository" commit --quiet -m mpv
git -C "$mpv_repository" -c tag.gpgsign=false tag release
mpv_commit="$(git -C "$mpv_repository" rev-parse HEAD)"

# git stays real, pointed at local repositories: the checkout is pinned by
# commit, and a stub that answered rev-parse would be asserting its own input.
shaderc_repository="$temporary/shaderc-source"
init_repository "$shaderc_repository"
mkdir -p "$shaderc_repository/utils"
printf '#!/bin/sh\nexit 0\n' >"$shaderc_repository/utils/git-sync-deps"
chmod +x "$shaderc_repository/utils/git-sync-deps"
git -C "$shaderc_repository" add utils/git-sync-deps
# Windows checkouts do not track the mode bit, and the build plan executes it.
git -C "$shaderc_repository" update-index --chmod=+x utils/git-sync-deps
git -C "$shaderc_repository" commit --quiet -m shaderc
git -C "$shaderc_repository" -c tag.gpgsign=false tag release
shaderc_commit="$(git -C "$shaderc_repository" rev-parse HEAD)"

dav1d_repository="$temporary/dav1d-source"
init_repository "$dav1d_repository"
printf 'stub dav1d\n' >"$dav1d_repository/meson.build"
git -C "$dav1d_repository" add meson.build
git -C "$dav1d_repository" commit --quiet -m dav1d
git -C "$dav1d_repository" -c tag.gpgsign=false tag release
dav1d_commit="$(git -C "$dav1d_repository" rev-parse HEAD)"

libplacebo_repository="$temporary/libplacebo-source"
init_repository "$libplacebo_repository"
printf 'stub libplacebo\n' >"$libplacebo_repository/meson.build"
git -C "$libplacebo_repository" add meson.build
git -C "$libplacebo_repository" commit --quiet -m libplacebo
git -C "$libplacebo_repository" -c tag.gpgsign=false tag release
libplacebo_commit="$(git -C "$libplacebo_repository" rev-parse HEAD)"

# nv-codec-headers is a Makefile that only installs headers and a .pc file;
# the recording make stub stands in for it.
nv_codec_headers_repository="$temporary/nv-codec-headers-source"
init_repository "$nv_codec_headers_repository"
printf 'stub nv-codec-headers\n' >"$nv_codec_headers_repository/Makefile"
git -C "$nv_codec_headers_repository" add Makefile
git -C "$nv_codec_headers_repository" commit --quiet -m nv-codec-headers
git -C "$nv_codec_headers_repository" -c tag.gpgsign=false tag release
nv_codec_headers_commit="$(git -C "$nv_codec_headers_repository" rev-parse HEAD)"

# libass builds autotools-style out of its fork checkout: the committed
# autogen.sh stands in for autoreconf and plants the recording configure stub,
# which is exactly the sequence the real build runs.
libass_repository="$temporary/libass-source"
init_repository "$libass_repository"
cat >"$libass_repository/autogen.sh" <<SH
#!/bin/sh
cp -- "$stub_extra/configure" configure
chmod +x configure
SH
chmod +x "$libass_repository/autogen.sh"
git -C "$libass_repository" add autogen.sh
git -C "$libass_repository" update-index --chmod=+x autogen.sh
git -C "$libass_repository" commit --quiet -m libass
git -C "$libass_repository" -c tag.gpgsign=false tag release
libass_commit="$(git -C "$libass_repository" rev-parse HEAD)"

# The stub manifest mirrors versions.json: linux pins live in overrides.linux
# wherever the real file keeps them there (libplacebo, whose base pin is the
# apple prebuilt), and those base fields carry broken values on purpose - a
# fold that stopped preferring the override would send the build to
# stub.invalid/wrong or a wrong directory name and fail loudly.
manifest="$temporary/versions.json"
cat >"$manifest" <<JSON
{
  "components": {
    "ffmpeg": {
      "kind": "git",
      "version": "$ffmpeg_version",
      "url": "file://$ffmpeg_repository",
      "ref": "release",
      "commit": "$ffmpeg_commit"
    },
    "dav1d": {
      "kind": "git",
      "version": "$dav1d_version",
      "url": "file://$dav1d_repository",
      "ref": "release",
      "commit": "$dav1d_commit"
    },
    "nv-codec-headers": {
      "kind": "git",
      "version": "$nv_codec_headers_version",
      "url": "file://$nv_codec_headers_repository",
      "ref": "release",
      "commit": "$nv_codec_headers_commit"
    },
    "shaderc": {
      "kind": "git",
      "version": "$shaderc_version",
      "url": "file://$shaderc_repository",
      "ref": "release",
      "commit": "$shaderc_commit"
    },
    "libplacebo": {
      "kind": "prebuilt",
      "version": "0.0.0",
      "url": "https://stub.invalid/wrong.zip",
      "overrides": {
        "linux": {
          "kind": "git",
          "version": "$libplacebo_version",
          "url": "file://$libplacebo_repository",
          "ref": "release",
          "commit": "$libplacebo_commit"
        }
      }
    },
    "libass": {
      "kind": "git",
      "version": "$libass_version",
      "url": "file://$libass_repository",
      "ref": "release",
      "commit": "$libass_commit"
    },
    "mpv": {
      "kind": "git",
      "version": "$mpv_version",
      "url": "file://$mpv_repository",
      "ref": "release",
      "commit": "$mpv_commit"
    }
  }
}
JSON

# An empty patches root: the plan must run its per-component series
# application, and a resolved empty series must be a clean no-op. The real
# pools stay out of it so this harness never depends on their contents.
patches_root="$temporary/patches-root"
mkdir -p "$patches_root"

# JOBS keeps nproc out of it; TMPDIR keeps main()'s own mktemp -d inside the
# directory this test already cleans up. Both runs below come through here so
# their environment cannot drift apart.
run_build() {
  VERSIONS_MANIFEST="$1" PATCHES_ROOT="$patches_root" PATH="$stub_bin:$PATH" \
    TMPDIR="$temporary/tmp" PREFIX="$temporary/prefix" JOBS=1 \
    bash "$SCRIPT_DIR/build.sh"
}

build_log="$temporary/build.log"
if ! run_build "$manifest" >"$build_log" 2>&1; then
  cat "$build_log" >&2
  fail "the stubbed build plan did not run to completion"
fi

# A pin whose kind stopped matching its acquisition step must fail at parse
# time, before anything is cloned - the acquisition steps are written per
# kind, and a silently retargeted step is exactly what resolve_pins' kind
# check exists to catch. Give mpv a linux override that swaps its tag for a
# release archive.
kind_manifest="$temporary/versions-kind-drift.json"
python3 - "$manifest" "$kind_manifest" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    manifest = json.load(source)
manifest["components"]["mpv"]["overrides"] = {
    "linux": {
        "kind": "archive",
        "url": "https://stub.invalid/mpv.tar.gz",
        "sha256": "0" * 64,
    }
}
with open(sys.argv[2], "w", encoding="utf-8") as sink:
    json.dump(manifest, sink)
PY
if run_build "$kind_manifest" >"$temporary/kind-drift.log" 2>&1; then
  fail "a kind drifted away from its acquisition step and was accepted"
fi
grep -q "kind 'archive'" "$temporary/kind-drift.log" ||
  fail "kind drift was not reported"

recorded_call() {
  local program="$1" directory="$2" candidate
  for candidate in "$records"/record.*; do
    [ -f "$candidate" ] || continue
    if [ "$(sed -n 1p "$candidate")" = "$program" ] &&
      [ "$(basename -- "$(sed -n 2p "$candidate")")" = "$directory" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

# -Fxq, so this matches a whole recorded argument. A substring search would
# accept -Dwayland=enabled inside a comment, which is exactly the hole here.
assert_argument() {
  local record="$1" argument="$2" description="$3"
  tail -n +3 "$record" | grep -Fxq -- "$argument" ||
    fail "$description does not pass $argument"
}

mpv_meson="$(recorded_call meson "mpv-$mpv_version")" ||
  fail "the build plan never ran meson in the mpv source tree"
[ "$(sed -n 3p "$mpv_meson")" = "setup" ] ||
  fail "mpv's first meson call is no longer 'setup'"

assert_argument "$mpv_meson" "-Dwayland=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dx11=disabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dvaapi=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dgl=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Degl=enabled" "mpv's meson setup"

# The DRM-derived providers are the ones that keep hardware decoding alive when
# the Wayland VA display cannot be had: vaapi-copy opens the render node on its
# own, and the GL dmabuf interop gives direct vaapi a display-independent
# path. mpv auto-disables the whole `drm` feature when libdisplay-info is
# missing, so these are pinned enabled and asserted here.
assert_argument "$mpv_meson" "-Ddrm=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dvaapi-drm=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dvaapi-wayland=enabled" "mpv's meson setup"

# NVIDIA GPUs have no VA driver out of the box, so nvdec is their only hardware
# decoder. Both features need ffnvcodec and quietly resolve to disabled without
# it, which lands every NVIDIA machine on software decoding.
assert_argument "$mpv_meson" "-Dcuda-hwaccel=enabled" "mpv's meson setup"
assert_argument "$mpv_meson" "-Dcuda-interop=enabled" "mpv's meson setup"

# vaapi and nvdec have to reach ffmpeg too, or mpv's hwdec has no decoder
# behind them (nvdec additionally needs ffnvcodec itself enabled). And
# the bundled ffmpeg needs a software AV1 decoder: its native av1 codec is
# hardware-accelerated only, so without dav1d an AV1 source on a machine whose
# GPU cannot decode it fails every packet and plays black video with audio.
ffmpeg_configure="$(recorded_call configure "ffmpeg-$ffmpeg_version")" ||
  fail "the build plan never configured ffmpeg"
assert_argument "$ffmpeg_configure" "--enable-vaapi" "ffmpeg's configure"
assert_argument "$ffmpeg_configure" "--enable-libdav1d" "ffmpeg's configure"
assert_argument "$ffmpeg_configure" "--enable-ffnvcodec" "ffmpeg's configure"
assert_argument "$ffmpeg_configure" "--enable-nvdec" "ffmpeg's configure"

# ffnvcodec is header-only: it must land in the prefix that ffmpeg's configure
# and mpv's meson resolve pkg-config against.
nv_codec_headers_make="$(recorded_call make "nv-codec-headers-$nv_codec_headers_version")" ||
  fail "the build plan never installed nv-codec-headers"
assert_argument "$nv_codec_headers_make" "PREFIX=$(realpath "$temporary/prefix")" "nv-codec-headers' make"
assert_argument "$nv_codec_headers_make" "install" "nv-codec-headers' make"

# dav1d must be built static so ffmpeg absorbs it into the bundled libmpv:
# a shared dav1d would have to travel in the bundle and be kept version-true
# with the host, which is exactly the coupling the rest of the plan avoids.
dav1d_meson="$(recorded_call meson "dav1d-v$dav1d_version")" ||
  fail "the build plan never ran meson in the dav1d source tree"
assert_argument "$dav1d_meson" "--default-library=static" "dav1d's meson setup"

# libplacebo runs meson as well, and its call carries no Wayland flag: finding
# it separately is what proves the directory above really discriminated.
recorded_call meson "libplacebo-v$libplacebo_version" >/dev/null ||
  fail "the build plan never ran meson in the libplacebo source tree"

# libass is the fork the whole platform set renders subtitles with, so the
# build must configure OUR checkout - the directory pins that - and must pin
# static, PIC and fontconfig rather than trust autodetection, for the same
# silent-degradation reasons as mpv's feature flags.
libass_configure="$(recorded_call configure "libass-$libass_version")" ||
  fail "the build plan never configured libass"
assert_argument "$libass_configure" "--enable-static" "libass's configure"
assert_argument "$libass_configure" "--disable-shared" "libass's configure"
assert_argument "$libass_configure" "--with-pic" "libass's configure"
assert_argument "$libass_configure" "--enable-fontconfig" "libass's configure"

echo "Linux native acquisition and build plan verification passed"
