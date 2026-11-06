#!/usr/bin/env bash
set -euo pipefail

. ../../include/path.sh

if [ "$1" = "build" ]; then
	true
elif [ "$1" = "clean" ]; then
	rm -rf _build$ndk_suffix
	exit 0
else
	exit 255
fi

mkdir -p _build$ndk_suffix
cd _build$ndk_suffix

cpu=armv7-a
[[ "$ndk_triple" == "aarch64"* ]] && cpu=armv8-a
[[ "$ndk_triple" == "x86_64"* ]] && cpu=generic
[[ "$ndk_triple" == "i686"* ]] && cpu="i686 --disable-asm"

cpuflags=
[[ "$ndk_triple" == "arm"* ]] && cpuflags="$cpuflags -mfpu=neon -mcpu=cortex-a8"

../configure \
	--target-os=android --enable-cross-compile --cross-prefix=$ndk_triple- --cc=$CC \
	--arch=${ndk_triple%%-*} --cpu=$cpu --pkg-config=pkg-config --nm=llvm-nm \
	--extra-cflags="-I$prefix_dir/include $cpuflags" --extra-ldflags="-L$prefix_dir/lib" \
	--extra-libs=-ldovi \
	--enable-{jni,mediacodec,openssl,libdav1d,libdovi} --disable-vulkan \
	--disable-static --enable-shared --enable-{gpl,version3} \
	--disable-{stripping,doc,programs} \
	--disable-{muxers,encoders,devices,filters} \
	--enable-muxer=spdif \
	--enable-filter=bwdif,decimate,fieldmatch,loudnorm,aresample,acompressor,alimiter \
	--disable-v4l2-m2m

make -j$cores
make DESTDIR="$prefix_dir" install

# libdovi.sh keeps libdovi's Rust runtime intrinsics to itself; a library
# that still carries them took memcpy & co. from Rust's generic routines.
for lib in "$prefix_dir"/lib/lib{avcodec,avdevice,avfilter,avformat,avutil,swresample,swscale}.so; do
	if llvm-nm "$lib" | grep compiler_builtins >/dev/null; then
		echo >&2 "$lib links Rust's compiler_builtins (see libdovi.sh)"
		exit 1
	fi
done
