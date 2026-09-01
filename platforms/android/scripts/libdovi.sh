#!/usr/bin/env bash
set -euo pipefail

. ../../include/path.sh

if [ "$1" = "build" ]; then
	true
elif [ "$1" = "clean" ]; then
	rm -f "$prefix_dir"/lib/libdovi.a "$prefix_dir"/include/libdovi/rpu_parser.h
	exit 0
else
	exit 255
fi

# Prebuilt static library (downloaded by download.sh); the header is vendored
# in the repo because the release tarballs ship only the archive.
# The release artifacts are named by Rust target triple, which differs from
# the NDK triple only for 32-bit ARM.
rust_triple=$ndk_triple
if [ "$ndk_triple" = "arm-linux-androideabi" ]; then
	rust_triple=armv7-linux-androideabi
fi
mkdir -p "$prefix_dir/lib" "$prefix_dir/include/libdovi"
install -m644 "$rust_triple/libdovi.a" "$prefix_dir/lib/libdovi.a"
install -m644 ../../include/libdovi/rpu_parser.h "$prefix_dir/include/libdovi/rpu_parser.h"
