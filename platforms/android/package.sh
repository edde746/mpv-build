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
#   include/libav*/, include/libsw*/
#                 the installed headers of the ffmpeg libraries in lib/, from
#                 the patched source (libavcodec/jni.h declares
#                 av_jni_set_mediacodec_callback_class, which the android
#                 series adds): what the mpv-android AAR's JNI glue
#                 (platforms/android/lib) and any other native consumer of
#                 these libraries compile against, so nobody vendors them
#   licenses/<component>/
#                 the licence texts of every component linked into lib/,
#                 copied from the sources download.sh fetched (deps/<name>,
#                 libdovi with the crates it links), for libc++ from the NDK
#                 and for rust-std (the Rust standard library in libdovi.a)
#                 from the pinned Rust toolchain; packaging fails when one of
#                 them has none
#
# The mpv-android AAR (platforms/android/lib/build-aar.sh) is assembled from
# these tarballs; a consumer that needs only the native trees unpacks them
# directly.
#
# Usage: package.sh [--arch <arch>]   (default: all four)
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
root="$( cd ../.. && pwd )"

. ./include/depinfo.sh
. ./include/loadarch.sh
. ./include/path.sh

# libdovi's crates are read through cargo, at the compiler the build used,
# whose standard library libdovi.a carries (rust-std, its notices).
RUSTUP_TOOLCHAIN="$(sed -n 's/^rust=//p' "$root/toolchain/android.txt")"
export RUSTUP_TOOLCHAIN
sources=("rust-std=$(rustc --print sysroot)/share/doc/rust")
for component in $components; do
	sources+=("$component=deps/$component")
done

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
ffmpeg_headers=(libavcodec libavdevice libavfilter libavformat libavutil
                libswresample libswscale)

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
	for dir in "${ffmpeg_headers[@]}"; do
		cp -R "prefix/$prefix_name/include/$dir" "$staging/include/"
	done
	python3 "$root/scripts/licenses.py" collect "$staging/licenses" "${sources[@]}" "libc++=$toolchain"
	python3 "$root/scripts/licenses.py" check "$staging/licenses" --group android libc++ rust-std
	asset="libmpv-android-$prefix_name.tar.gz"
	# Reproducible bytes: the release skips re-uploading an existing
	# content-addressed name, so a rebuild of the same key MUST produce the
	# identical archive or the recorded checksum drifts from the published
	# bytes. Fixed order, owners and mtimes; gzip -n drops its own timestamp.
	tar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime='2020-01-01 00:00:00 UTC' \
		-C "$staging" -cf - lib include licenses | gzip -n -9 > "$release_dir/$asset"
	rm -rf "$staging"
	echo "packaged $asset"
done
