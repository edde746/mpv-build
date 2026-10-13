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

# ffmpeg's TLS backend. OpenSSL's AES-GCM assembly works on many blocks per
# call and picks the Armv8 AES/PMULL instructions at run time, which also
# reaches the 32-bit userland most Android TV devices run on Armv8 cores.
# mbedTLS offered ChaCha20-Poly1305 first and ran AES-GCM one 16-byte block
# per call: on a Fire TV Stick 4K Max core libavformat's HTTPS read path cost
# 31.8 cycles/byte (24.5 with AES-GCM forced) against 9.2 here (plezy#2643).
case "$ndk_triple" in
	arm*) target=android-arm ;;
	aarch64*) target=android-arm64 ;;
	i686*) target=android-x86 ;;
	x86_64*) target=android-x86_64 ;;
esac

# The android targets find clang through the NDK root, which path.sh unsets,
# and name the compiler after __ANDROID_API__, so pass loadarch's API level.
export ANDROID_NDK_ROOT="$sdk_root/ndk/$v_ndk"
api=${CC%-clang}
api=${api##*[a-z]}

mkdir -p _build$ndk_suffix
cd _build$ndk_suffix

# Static, no config file, modules, dlopen, apps or tests: ffmpeg's tls and
# dtls protocols and av_random_bytes are the only callers. The algorithm and
# protocol trims are the windows group's (winbuild's packages/openssl.cmake).
../Configure "$target" -D__ANDROID_API__=$api \
	--prefix=/usr --libdir=lib --release \
	no-shared no-module no-legacy no-dso no-autoload-config \
	no-apps no-tests no-docs no-filenames no-err \
	no-ssl3 no-ssl3-method no-ocsp no-cms no-cmp no-srp no-dh no-dsa no-cmac \
	no-whirlpool no-rmd160 no-md4 no-mdc2 \
	no-camellia no-aria no-seed no-idea no-cast no-bf no-rc2 no-rc4 \
	no-sm2 no-sm3 no-sm4

# OpenSSL stamps "built on: <date>" into libcrypto (OpenSSL_version), and
# libavformat links it statically, so every rebuild of a published key gave
# different bytes: lld places merged strings by content hash, so that one
# string moved most of .rodata and every reference to it. Its buildinf.h
# generator takes SOURCE_DATE_EPOCH instead of the clock; 2020-01-01 is the
# mtime the packaged archives carry.
SOURCE_DATE_EPOCH=1577836800 make -j$cores build_sw
make DESTDIR="$prefix_dir" install_sw
