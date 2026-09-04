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

# Build the patched parser from the pinned source, never the old prebuilts.
if [ ! -f dolby_vision/Cargo.toml ]; then
	echo >&2 "libdovi source missing; move obsolete deps/libdovi aside and rerun download.sh"
	exit 1
fi
if ! command -v rustup >/dev/null; then
	echo >&2 "rustup is required to build libdovi (https://rustup.rs)"
	exit 1
fi
rust_version="$(sed -n 's/^rust=//p' "$DIR/../../toolchain/android.txt")"
if [ -z "$rust_version" ]; then
	echo >&2 "toolchain/android.txt: missing rust= line"
	exit 1
fi

# Rust names 32-bit ARM differently from the NDK.
rust_triple=$ndk_triple
if [ "$ndk_triple" = "arm-linux-androideabi" ]; then
	rust_triple=armv7-linux-androideabi
fi
rustup toolchain install "$rust_version" --profile minimal --no-self-update
rustup target add --toolchain "$rust_version" "$rust_triple"
cargo "+$rust_version" rustc --locked --release \
	--manifest-path dolby_vision/Cargo.toml --features capi \
	--target "$rust_triple" --target-dir target --crate-type staticlib \
	-- -C panic=abort -C relocation-model=pic
mkdir -p "$prefix_dir/lib" "$prefix_dir/include/libdovi"
install -m644 "target/$rust_triple/release/libdolby_vision.a" "$prefix_dir/lib/libdovi.a"
install -m644 ../../include/libdovi/rpu_parser.h "$prefix_dir/include/libdovi/rpu_parser.h"
