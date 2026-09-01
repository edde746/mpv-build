#!/usr/bin/env bash
# Per-ABI release tarballs for the libmpv-android artifact.
#
# For each ABI this produces dist/release/libmpv-android-<key>-<abi>.tar.gz
# (the group.json assetPattern; the key comes from keys.py, so the name is
# content-addressed and immutable) containing:
#
#   lib/          libmpv.so, the ffmpeg shared libraries (libav*, libsw*,
#                 libpostproc) and the pinned NDK's libc++_shared.so (built
#                 with -Wl,-z,max-page-size=16384 throughout; Plezy ships this
#                 exact libc++ per ABI)
#   include/mpv/  client.h render.h render_gl.h stream_cb.h, as installed
#                 into the prefix from the pinned mpv source by the build
#
# This replaces the fork's Gradle AAR assembly (scripts/mpv-android.sh): the
# consumer unpacks these trees directly.
#
# Usage: package.sh [--arch <armv7l|arm64|x86|x86_64>]   (default: all four)
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
root="$( cd ../.. && pwd )"

arch=
while [ $# -gt 0 ]; do
	case "$1" in
		--arch)
		shift
		arch=$1
		;;
		*)
		echo >&2 "usage: package.sh [--arch <armv7l|arm64|x86|x86_64>]"
		exit 1
		;;
	esac
	shift
done

archs=(armv7l arm64 x86 x86_64)
if [ -n "$arch" ]; then
	archs=("$arch")
fi

key="$( cd "$root" && python3 scripts/keys.py keys --platform-group android \
	| python3 -c 'import json, sys; print(json.load(sys.stdin)["libmpv-android"])' )"

. ./include/loadarch.sh
. ./include/path.sh

release_dir="$root/dist/release"
mkdir -p "$release_dir"

# No libpostproc: FFmpeg 8.0 removed it upstream (the fork's AAR-era symlink
# list predates that and would dangle against the n8.0.1 pin).
libs=(libmpv.so libavcodec.so libavdevice.so libavfilter.so libavformat.so
      libavutil.so libswresample.so libswscale.so)
headers=(client.h render.h render_gl.h stream_cb.h)

for arch in "${archs[@]}"; do
	loadarch "$arch"
	staging="$(mktemp -d)"
	mkdir -p "$staging/lib" "$staging/include/mpv"
	for lib in "${libs[@]}"; do
		cp "prefix/$prefix_name/lib/$lib" "$staging/lib/"
	done
	cp "$toolchain/sysroot/usr/lib/$ndk_triple/libc++_shared.so" "$staging/lib/"
	for header in "${headers[@]}"; do
		cp "prefix/$prefix_name/include/mpv/$header" "$staging/include/mpv/"
	done
	asset="libmpv-android-$key-$prefix_name.tar.gz"
	tar -czf "$release_dir/$asset" -C "$staging" lib include
	rm -rf "$staging"
	echo "packaged $asset"
done
