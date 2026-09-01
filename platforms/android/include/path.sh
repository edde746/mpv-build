# Host and toolchain environment, sourced by the driver entry points and by
# every per-dep build script (from deps/<name>, hence the ../.. paths).
#
# The NDK version is pinned in toolchain/android.txt (part of every content
# key). The SDK root defaults to the tree download-sdk.sh bootstraps under
# platforms/android/sdk/; set MPV_ANDROID_SDK to an existing SDK root (e.g.
# ~/Library/Android/sdk) to use a preinstalled copy of the pinned NDK instead.

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && cd .. && pwd )"

os=linux
if [[ "$OSTYPE" == "darwin"* ]]; then
	os=mac
fi
export os

if [ "$os" = "mac" ]; then
	if [ -z "${cores:-}" ]; then
		cores=$(sysctl -n hw.ncpu)
	fi
	# various things rely on GNU behaviour
	INSTALL="$(command -v ginstall || true)"
	export INSTALL
	export SED=gsed
else
	if [ -z "${cores:-}" ]; then
		cores=$(grep -c ^processor /proc/cpuinfo)
	fi
fi
cores=${cores:-4}
export cores

v_ndk="$(sed -n 's/^ndk=//p' "$DIR/../../toolchain/android.txt")"
if [ -z "$v_ndk" ]; then
	echo >&2 "toolchain/android.txt: missing ndk= line"
	exit 1
fi

# configure pkg-config paths once an ABI has been loaded
if [ -n "${ndk_triple:-}" ]; then
	export PKG_CONFIG_SYSROOT_DIR="$prefix_dir"
	export PKG_CONFIG_LIBDIR="$PKG_CONFIG_SYSROOT_DIR/lib/pkgconfig"
	unset PKG_CONFIG_PATH
fi

sdk_root="${MPV_ANDROID_SDK:-$DIR/sdk/android-sdk-$os}"
toolchain=$(echo "$sdk_root/ndk/$v_ndk/toolchains/llvm/prebuilt/"*)
export PATH="$toolchain/bin:$sdk_root/ndk/$v_ndk:$PATH"
export ANDROID_HOME="$sdk_root"
unset ANDROID_SDK_ROOT ANDROID_NDK_ROOT
