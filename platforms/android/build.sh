#!/usr/bin/env bash
# Android build driver: cross-compiles the libmpv-android dependency graph per
# ABI into prefix/<abi>/, using the NDK pinned in toolchain/android.txt.
# Sources come from ./download.sh (pinned + patched); the per-ABI release
# tarballs come from ./package.sh afterwards.
#
# Usage: build.sh [options] [target]
#   -n / --no-deps   do not build dependencies first
#   --clean          clean build dirs before compiling
#   --arch <arch>    build one architecture (armv7l, arm64, x86, x86_64);
#                    default is all four
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
. ./include/depinfo.sh
. ./include/loadarch.sh

cleanbuild=0
nodeps=0
target=libmpv-android
archs=(armv7l arm64 x86 x86_64)
arch=

getdeps () {
	# dep_<target> with dashes as underscores; unset-safe under set -u.
	eval "echo \${dep_${1//-/_}[*]-}"
}

build () {
	if [ "$1" != "libmpv-android" ] && [ ! -d "deps/$1" ]; then
		printf >&2 '\e[1;31m%s\e[m\n' "Target $1 not found (run ./download.sh first)"
		return 1
	fi
	if [ $nodeps -eq 0 ]; then
		printf >&2 '\e[1;34m%s\e[m\n' "Preparing $1..."
		local deps dep
		deps=$(getdeps "$1")
		if [ -n "$deps" ]; then
			echo >&2 "Dependencies: $deps"
		fi
		for dep in $deps; do
			build "$dep"
		done
	fi
	if [ "$1" != "libmpv-android" ]; then
		printf >&2 '\e[1;34m%s\e[m\n' "Building $1..."
		pushd "deps/$1" >/dev/null
		BUILDSCRIPT=../../scripts/$1.sh
		if [ $cleanbuild -eq 1 ]; then
			$BUILDSCRIPT clean
		fi
		$BUILDSCRIPT build
		popd >/dev/null
	fi
}

usage () {
	printf '%s\n' \
		"Usage: build.sh [options] [target]" \
		"Builds the specified target (default: $target)" \
		"-n             Do not build dependencies" \
		"--clean        Clean build dirs before compiling" \
		"--arch <arch>  Build for specified architecture (supported: armv7l, arm64, x86, x86_64)"
	exit 0
}

while [ $# -gt 0 ]; do
	case "$1" in
		--clean)
		cleanbuild=1
		;;
		-n|--no-deps)
		nodeps=1
		;;
		--arch)
		shift
		arch=$1
		;;
		-h|--help)
		usage
		;;
		*)
		target=$1
		;;
	esac
	shift
done

if [ -z "$arch" ]; then
	for arch in "${archs[@]}"; do
		loadarch "$arch"
		setup_prefix
		build "$target"
	done
else
	loadarch "$arch"
	setup_prefix
	build "$target"
fi

exit 0
