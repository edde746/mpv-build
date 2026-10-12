#!/usr/bin/env bash
set -euo pipefail

. ../../include/path.sh

if [ "$1" = "build" ]; then
	true
elif [ "$1" = "clean" ]; then
	make clean
	exit 0
else
	exit 255
fi

$0 clean # separate building not supported, always clean

# armeabi-v7a mostly runs on Armv8 CPUs with a 32-bit userland (Fire TV 4K
# sticks, the Google TV Streamer, MT8696 boxes), whose AES and PMULL
# instructions mbedTLS only compiles in when the target is Armv8. Built for
# armv7-a, AES-GCM runs as table-driven C: ~120 Mbps on a 2 GHz Cortex-A55
# core, which an HTTPS UHD remux exceeds (plezy#2643). So aesce.c alone is
# compiled for Armv8, and the rest of the library, still armv7-a, is told the
# AESCE dispatch exists. The AES code runs only after aesce.c's
# getauxval(HWCAP2_AES) check passes; that check is plain integer code, so
# Armv7-only CPUs keep the C path.
if [[ "$ndk_triple" == "arm"* ]]; then
	export CFLAGS="-O2 -DMBEDTLS_ARCH_IS_ARMV8_A"
	make -C library CFLAGS="$CFLAGS -march=armv8-a -mfpu=crypto-neon-fp-armv8" aesce.o
fi

make -j$cores no_test
make DESTDIR="$prefix_dir" install
