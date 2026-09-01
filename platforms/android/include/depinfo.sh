# Dependency graph of the android build, sourced by build.sh.
#
# Versions live in the repo-root versions.json (override-aware for the android
# group); this file only says what depends on what. Component names are the
# canonical versions.json names, which are also the deps/<name> directories
# and the scripts/<name>.sh build scripts.
#
# Arrays because bash has no dict-of-arrays; build.sh's getdeps() expands
# dep_<target> (dashes become underscores) with a set -u-safe default.

dep_mbedtls=()
dep_dav1d=()
dep_libdovi=()
dep_ffmpeg=(mbedtls dav1d libdovi)
dep_freetype=()
dep_fribidi=()
dep_harfbuzz=()
dep_libunibreak=()
dep_libass=(freetype fribidi harfbuzz libunibreak)
dep_lua=()
dep_libplacebo=()
dep_mpv=(ffmpeg libass lua libplacebo)
dep_libmpv_android=(mpv)
