#!/usr/bin/env bash
# Windows platform driver: build the libmpv dev package with mpv-winbuild-cmake.
#
# usage: platforms/windows/build.sh <x86_64|aarch64> [--configure-only]
#
# Stages:
#   1. shallow-fetch shinchiro/mpv-winbuild-cmake at the commit pinned by the
#      versions.json component `mpv-winbuild-cmake`
#   2. pin packages/{mpv,ffmpeg,libass}.cmake to versions.json, stage the
#      resolved windows patch series and neutralize the check-git cache
#      cascade (pin_packages.py; libass then builds from our edde746/libass
#      fork)
#   3. cmake configure with the clang toolchain (aarch64 REQUIRES clang:
#      gcc + aarch64 is a configure-time FATAL_ERROR upstream) and ccache
#      baked into the cross-compiler wrappers. winbuild's
#      cmake/download_externalproject.cmake curls its patched Kitware
#      ExternalProject module at first configure, so configure needs network.
#   4. toolchain bootstrap: ninja llvm, ninja rustup, ninja llvm-clang --
#      skipped entirely when the per-arch tree's marker says the restored CI
#      cache already holds this generation + winbuild pin
#   5. ninja mpv, after fullcleaning any restored mpv state -- meson
#      -Dlibmpv=true is hard-coded upstream, so this builds libmpv-2.dll
#      alongside mpv.exe and its copy-binary step assembles
#      mpv-dev-<cpu>-<date>-git-<10sha>/ (libmpv-2.dll, libmpv.dll.a,
#      include/mpv/{client,stream_cb,render,render_gl}.h) in the build root.
#      package.sh zips that tree under its content-addressed asset name.
#
# Host requirements for a full build: Linux with the mpv-winbuild-cmake README
# apt package list plus pip meson >= 1.3.0 (configure fails fast otherwise).
# CI is where full builds run. On any host, --configure-only stops after the
# cmake configure step; it exists to smoke-test the rewritten package files
# through the real configure (e.g. inside a docker ubuntu container).
set -euo pipefail

usage() {
  echo "usage: platforms/windows/build.sh <x86_64|aarch64> [--configure-only]" >&2
  exit 2
}

ARCH="${1:-}"
case "$ARCH" in
  x86_64 | aarch64) ;;
  *) usage ;;
esac
CONFIGURE_ONLY=0
if [[ "${2:-}" == "--configure-only" ]]; then
  CONFIGURE_ONLY=1
elif [[ -n "${2:-}" ]]; then
  usage
fi

# winbuild's own patch steps run `git am` (spirv-cross, fontconfig, ...),
# which hard-fails without a committer identity -- upstream's CI configures
# one globally as its first step. Scope ours to this process instead of
# mutating the host's git config.
export GIT_AUTHOR_NAME="${GIT_AUTHOR_NAME:-mpv-build}"
export GIT_AUTHOR_EMAIL="${GIT_AUTHOR_EMAIL:-mpv-build@localhost}"
export GIT_COMMITTER_NAME="${GIT_COMMITTER_NAME:-$GIT_AUTHOR_NAME}"
export GIT_COMMITTER_EMAIL="${GIT_COMMITTER_EMAIL:-$GIT_AUTHOR_EMAIL}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

# The winbuild pin, override-aware for the windows group.
read -r WINBUILD_URL WINBUILD_COMMIT < <(python3 - "$ROOT/versions.json" <<'PY'
import json, sys
entry = json.load(open(sys.argv[1]))["components"]["mpv-winbuild-cmake"]
pins = {f: entry[f] for f in ("url", "commit") if f in entry}
pins.update({f: v for f, v in (entry.get("overrides", {}).get("windows") or {}).items()
             if f in ("url", "commit")})
print(pins["url"], pins["commit"])
PY
)

WORK="$ROOT/build/windows"
SRC="$WORK/mpv-winbuild-cmake"
BUILD="$WORK/$ARCH"
mkdir -p "$WORK"

echo "==> mpv-winbuild-cmake @ $WINBUILD_COMMIT"
if [[ ! -d "$SRC/.git" ]]; then
  # The unified repo may itself be a git worktree whose admin dir sits outside
  # a container mount (e.g. this repo checked out via `git worktree`); a plain
  # `git init` under it would discover upward, hit the unresolvable gitdir
  # pointer and die. The ceiling confines discovery to the new repo itself.
  GIT_CEILING_DIRECTORIES="$SRC" git init -q "$SRC"
  git -C "$SRC" remote add origin "$WINBUILD_URL"
fi
if ! git -C "$SRC" cat-file -e "$WINBUILD_COMMIT^{commit}" 2> /dev/null; then
  # GitHub allows fetching an arbitrary commit by sha, so the shallow fetch
  # follows the pin instead of a branch tip.
  git -C "$SRC" fetch --depth 1 origin "$WINBUILD_COMMIT"
fi
git -C "$SRC" -c advice.detachedHead=false checkout -q --detach "$WINBUILD_COMMIT"
# Discard rewrites from a previous run; pin_packages.py re-applies from the
# current versions.json, and its patch staging cleans up after itself.
git -C "$SRC" reset --hard -q "$WINBUILD_COMMIT"

echo "==> pinning packages to versions.json"
python3 "$HERE/pin_packages.py" --winbuild "$SRC" --repo "$ROOT"

echo "==> configuring for $ARCH-w64-mingw32 (clang toolchain)"
# SINGLE_SOURCE_LOCATION and RUSTUP_LOCATION sit outside the per-arch build
# dir so the x86_64 and aarch64 legs share sources and the rust toolchain.
# ENABLE_CCACHE bakes ccache into the generated cross-compiler wrappers with
# its cache inside the CI-cached install prefix; winbuild silently disables
# it when no ccache binary is on the host, so --configure-only hosts need
# nothing extra. It absorbs the rebuild cost of the packages a pin bump
# dirties on a warm tree (and of bootstrap retries after a timeout).
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DTARGET_ARCH="$ARCH-w64-mingw32" \
  -DCOMPILER_TOOLCHAIN=clang \
  -DENABLE_CCACHE=ON \
  -DCCACHE_MAXSIZE=2G \
  -DSINGLE_SOURCE_LOCATION="$WORK/src" \
  -DRUSTUP_LOCATION="$WORK/rustup"

if [[ "$CONFIGURE_ONLY" == 1 ]]; then
  echo "==> --configure-only: stopping after cmake configure"
  exit 0
fi

if [[ "$(uname -s)" != Linux ]]; then
  echo "error: the toolchain bootstrap and package builds only run on a Linux host (CI); use --configure-only elsewhere" >&2
  exit 1
fi

# CI restores the whole build state (per-arch tree + shared src + rustup) as
# cache entries. The toolchain is fully described by the generation in
# toolchain/windows.txt plus the winbuild commit, so when the marker written
# after the last successful bootstrap matches both, skip the three targets
# outright: driving them on a warm tree re-runs upstream's always-dirty
# check-git steps and, worst case, re-drives llvm. On a mismatch (a tree
# restored across a winbuild bump) the targets run and converge
# incrementally: unchanged steps are stamp-clean, changed recipes are dirtied
# by their new command lines.
TOOLCHAIN_MARKER="$BUILD/.toolchain-bootstrapped"
TOOLCHAIN_STATE="$(grep -E '^generation=' "$ROOT/toolchain/windows.txt") winbuild=$WINBUILD_COMMIT"
if [[ "$(cat "$TOOLCHAIN_MARKER" 2> /dev/null)" == "$TOOLCHAIN_STATE" ]]; then
  echo "==> toolchain already bootstrapped for this generation+winbuild pin; skipping"
  # rustup travels in its own cache path and can be evicted independently of
  # the per-arch stamps that claim it was installed; when the binary is gone,
  # fullclean the package so it reinstalls instead of every rust-built
  # dependency failing later with a missing cargo.
  if [[ ! -x "$WORK/rustup/.cargo/bin/cargo" ]]; then
    echo "==> rustup missing from the shared cache; reinstalling"
    ninja -C "$BUILD" rustup-fullclean
    ninja -C "$BUILD" rustup
  fi
else
  echo "==> bootstrapping toolchain (llvm, rustup, llvm-clang)"
  rm -f "$TOOLCHAIN_MARKER"
  ninja -C "$BUILD" llvm
  ninja -C "$BUILD" rustup
  ninja -C "$BUILD" llvm-clang
  printf '%s\n' "$TOOLCHAIN_STATE" > "$TOOLCHAIN_MARKER"
fi

# mpv is rebuilt from scratch on every run. A restored tree may carry clean
# mpv stamps whose build directory upstream's postremovebuild step already
# deleted, and upstream's packaging steps embed BUILDDATE in their command
# lines, so a partially-dirty mpv would try to re-run copy steps against a
# build tree that no longer exists. Any run that reaches this point has a
# stale windows key, which always requires relinking libmpv anyway.
# fullclean's git commands need the mpv source; when the shared source cache
# was evicted independently of the per-arch stamps, drop mpv's prefix
# (stamps and logs only -- the source lives under src/) so the whole chain
# re-clones and rebuilds instead of trusting stamps for a source that is
# gone.
echo "==> building mpv (produces the libmpv dev package)"
if [[ -d "$WORK/src/mpv" ]]; then
  ninja -C "$BUILD" mpv-fullclean
else
  rm -rf "$BUILD/packages/mpv-prefix"
fi
rm -rf "$BUILD"/mpv-*
ninja -C "$BUILD" mpv

DEV_DIR="$(find "$BUILD" -maxdepth 1 -type d -name 'mpv-dev-*' | head -n 1)"
echo "==> done: ${DEV_DIR:-<mpv-dev tree not found in $BUILD>}"
echo "    next: platforms/windows/package.sh $ARCH"
