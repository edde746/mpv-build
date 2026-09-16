#!/usr/bin/env bash
# Per-ABI release tarballs for the libmpv-android artifact.
#
# For each ABI this produces dist/release/libmpv-android-<abi>.tar.gz, the
# unkeyed spelling keys.py record-platform renames to the group.json
# assetPattern's content-addressed name (the key covers the published driver,
# which the leg that builds a tarball cannot know is final yet). Contents:
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
# Usage: package.sh [--arch <arch>]   (default: all four)
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
root="$( cd ../.. && pwd )"

. ./include/loadarch.sh
. ./include/path.sh

arch=
while [ $# -gt 0 ]; do
	case "$1" in
		--arch)
		shift
		arch=$1
		;;
		*)
		echo >&2 "usage: package.sh [--arch <${android_arch_list//, /|}>]"
		exit 1
		;;
	esac
	shift
done

archs=("${android_arches[@]}")
if [ -n "$arch" ]; then
	archs=("$arch")
fi

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
	asset="libmpv-android-$prefix_name.tar.gz"
	# Reproducible bytes: the release skips re-uploading an existing
	# content-addressed name, so a rebuild of the same key MUST produce the
	# identical archive or the recorded checksum drifts from the published
	# bytes. Fixed order, owners and mtimes; gzip -n drops its own timestamp.
	tar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime='2020-01-01 00:00:00 UTC' \
		-C "$staging" -cf - lib include | gzip -n -9 > "$release_dir/$asset"
	rm -rf "$staging"
	echo "packaged $asset"
done
