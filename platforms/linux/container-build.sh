#!/usr/bin/env bash
set -euo pipefail

# Runs the linux build inside the pinned toolchain container from
# toolchain/linux.txt. This wrapper is the published path: CI calls it, and a
# local docker run through it produces the same glibc floor as CI. Running
# build.sh bare on some other Linux still works for development, but its
# output must never be published.
#
# Environment:
#   JOBS      forwarded to build.sh
#   KEY       when set, package.sh runs after the build and names the tarball
#   PLATFORM  docker --platform (e.g. linux/amd64); defaults to the host arch
#   PREFIX    in-container install prefix (default /work/libmpv-prefix); give
#             concurrent runs of different architectures distinct prefixes
#   OUT_DIR   in-container tarball directory (default /work/dist/release)
#
# The repo is mounted at /work and the build writes the prefix and, when
# packaging, the tarball directory into it (both gitignored).

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

image="$(sed -n 's/^image=//p' "$REPO_ROOT/toolchain/linux.txt")"
if [ -z "$image" ]; then
  echo "toolchain/linux.txt does not pin a container image" >&2
  exit 1
fi

# Everything the six build steps need, and nothing for the Flutter runner:
#   toolchain      build-essential git curl ca-certificates python3 pkg-config
#                  cmake meson ninja-build nasm autoconf automake libtool
#                  xz-utils (ffmpeg tarball) zstd (packaging)
#   libass         libfreetype-dev libfribidi-dev libharfbuzz-dev
#                  libfontconfig-dev (host text stack; see toolchain/linux.txt)
#   ffmpeg         libgnutls28-dev libva-dev
#   mpv            libdisplay-info-dev libdrm-dev libwayland-dev
#                  wayland-protocols libxkbcommon-dev libegl-dev libgl-dev
#                  libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev
#                  liblua5.2-dev libmujs-dev liblcms2-dev
DEPS='
build-essential git curl ca-certificates python3 pkg-config
cmake meson ninja-build nasm autoconf automake libtool xz-utils zstd
libfreetype-dev libfribidi-dev libharfbuzz-dev libfontconfig-dev
libgnutls28-dev libva-dev
libdisplay-info-dev libdrm-dev libwayland-dev wayland-protocols
libxkbcommon-dev libegl-dev libgl-dev libgbm-dev
libasound2-dev libpulse-dev libpipewire-0.3-dev
liblua5.2-dev libmujs-dev liblcms2-dev
'

command='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -o Acquire::Retries=3 -qq
# shellcheck disable=SC2086
apt-get install -y -o Acquire::Retries=3 -qq --no-install-recommends '"$(echo $DEPS)"'
bash platforms/linux/build.sh
if [ -n "${KEY:-}" ]; then
  bash platforms/linux/package.sh
fi
'

exec docker run --rm \
  ${PLATFORM:+--platform "$PLATFORM"} \
  --volume "$REPO_ROOT:/work" \
  --workdir /work \
  --env JOBS \
  --env KEY \
  --env ARCH \
  --env PREFIX \
  --env OUT_DIR \
  "$image" \
  bash -c "$command"
