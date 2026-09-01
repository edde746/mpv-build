# ABI selection and per-ABI prefix setup, sourced by build.sh and package.sh
# (both run with platforms/android as the working directory).

loadarch () {
	unset CC CXX CPATH LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
	unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

	# Android API level 25: Plezy ships to Fire OS 6 devices and pins
	# minSdk 25, so the native libraries must not require anything newer.
	# Upstream mpv-android historically built at 21, so 25 is conservative
	# room, not a floor raise. The libmpv-android fork's AAR era built at 26,
	# which forced a minSdk=26 override hack in the app; that hack dies with
	# this driver.
	local apilvl=25
	# ndk_triple: what the toolchain actually is
	# cc_triple: what Google pretends the toolchain is
	if [ "$1" = "armv7l" ]; then
		export ndk_suffix=
		export ndk_triple=arm-linux-androideabi
		cc_triple=armv7a-linux-androideabi$apilvl
		prefix_name=armeabi-v7a
	elif [ "$1" = "arm64" ]; then
		export ndk_suffix=-arm64
		export ndk_triple=aarch64-linux-android
		cc_triple=$ndk_triple$apilvl
		prefix_name=arm64-v8a
	elif [ "$1" = "x86" ]; then
		export ndk_suffix=-x86
		export ndk_triple=i686-linux-android
		cc_triple=$ndk_triple$apilvl
		prefix_name=x86
	elif [ "$1" = "x86_64" ]; then
		export ndk_suffix=-x64
		export ndk_triple=x86_64-linux-android
		cc_triple=$ndk_triple$apilvl
		prefix_name=x86_64
	else
		echo >&2 "Invalid architecture: $1 (supported: armv7l, arm64, x86, x86_64)"
		exit 1
	fi
	export prefix_name
	export prefix_dir="$PWD/prefix/$prefix_name"
	export CC=$cc_triple-clang
	export CXX=$cc_triple-clang++
	# --icf=safe folds identical code; -z,max-page-size=16384 keeps every .so
	# loadable on Android 15+ devices with 16 KiB pages (a Plezy packaging
	# invariant -- do not drop it).
	export LDFLAGS="-Wl,-O1,--icf=safe -Wl,-z,max-page-size=16384"
	export AR=llvm-ar
	export RANLIB=llvm-ranlib
}

setup_prefix () {
	if [ ! -d "$prefix_dir" ]; then
		mkdir -p "$prefix_dir"
		# enforce flat structure (/usr/local -> /)
		ln -s . "$prefix_dir/usr"
		ln -s . "$prefix_dir/local"
	fi

	local cpu_family=${ndk_triple%%-*}
	if [ "$cpu_family" = "i686" ]; then
		cpu_family=x86
	fi

	# meson wants to be spoonfed this file, so create it ahead of time
	# also define: release build, static libs and no source downloads at runtime(!!!)
	cat >"$prefix_dir/crossfile.txt" <<CROSSFILE
[built-in options]
prefix = '/usr'
buildtype = 'release'
b_ndebug = 'true'
default_library = 'static'
wrap_mode = 'nodownload'
[binaries]
c = '$CC'
cpp = '$CXX'
ar = 'llvm-ar'
nm = 'llvm-nm'
strip = 'llvm-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = '$cpu_family'
cpu = '${CC%%-*}'
endian = 'little'
CROSSFILE
}
