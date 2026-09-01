#!/usr/bin/env bash
# Bootstraps the host toolchain for the android driver: build tools from the
# system package manager, then Android commandline-tools + the exact NDK
# pinned in toolchain/android.txt.
#
# When MPV_ANDROID_SDK points at an existing SDK root that already carries the
# pinned NDK (e.g. ~/Library/Android/sdk), the whole bootstrap is skipped and
# path.sh uses that root directly.
#
# The Gradle/AAR era's platform, build-tools, cmake and JDK requirements are
# gone: only the NDK (plus meson/ninja/autotools/nasm on the host) is needed
# to cross-compile the native trees.
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )/.."
. ./include/path.sh # $os, $v_ndk

# Android commandline-tools archive (host bootstrap detail, not a content-key
# input by itself -- though this file is part of the driver digest).
v_cmdline_tools=14742923_latest

if [ -n "${MPV_ANDROID_SDK:-}" ]; then
	if [ -d "$MPV_ANDROID_SDK/ndk/$v_ndk" ]; then
		echo "using existing SDK at $MPV_ANDROID_SDK (ndk $v_ndk)"
		exit 0
	fi
	echo >&2 "MPV_ANDROID_SDK=$MPV_ANDROID_SDK does not contain ndk/$v_ndk"
	exit 1
fi

if [ "$os" = "linux" ]; then
	if hash yum &>/dev/null; then
		sudo yum install autoconf pkgconfig libtool ninja-build nasm \
			python3-pip python3-setuptools unzip wget
		python3 -m pip install meson jsonschema jinja2
	fi
	if apt-get -v &>/dev/null; then
		sudo apt-get update
		sudo apt-get install -y autoconf pkg-config libtool ninja-build nasm \
			python3-pip python3-setuptools unzip meson python3-jinja2
	fi
elif [ "$os" = "mac" ]; then
	if ! hash brew 2>/dev/null; then
		echo >&2 "Error: brew not found. You need to install Homebrew: https://brew.sh/"
		exit 255
	fi
	brew install \
		automake autoconf libtool pkg-config \
		coreutils gnu-sed wget meson ninja nasm
fi

mkdir -p sdk && cd sdk

if [ ! -d "android-sdk-${os}" ]; then
	curl -fsSLO "https://dl.google.com/android/repository/commandlinetools-${os}-${v_cmdline_tools}.zip"
	mkdir "android-sdk-${os}"
	unzip -q -d "android-sdk-${os}" "commandlinetools-${os}-${v_cmdline_tools}.zip"
	rm "commandlinetools-${os}-${v_cmdline_tools}.zip"
fi

sdkmanager () {
	local exe="./android-sdk-$os/cmdline-tools/latest/bin/sdkmanager"
	if [ ! -x "$exe" ]; then
		exe="./android-sdk-$os/cmdline-tools/bin/sdkmanager"
	fi
	"$exe" --sdk_root="${ANDROID_HOME}" "$@"
}

echo y | sdkmanager "ndk;${v_ndk}"

cd ..
