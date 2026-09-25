# Per-component build table for the android build, sourced by build.sh
# (dep_/recipe_), download.sh (components) and the scripts/*.sh drivers
# (args_/pc_libs_).
#
# Versions live in the repo-root versions.json (override-aware for the android
# group); this file says what depends on what, which driver builds a component
# and with what flags. Component names are the canonical versions.json names,
# which are also the deps/<name> directories.
#
#   components     every component, in the order download.sh fetches them
#   dep_<name>     components that have to be built before this one
#   recipe_<name>  the scripts/<recipe>.sh that builds it, called as
#                  <recipe>.sh <build|clean> <name>
#   args_<name>    the extra flags that driver takes, unsplit
#   pc_libs_<name> what meson.sh appends to the component's pkg-config Libs: line
#
# A <name> with a dash is spelled with an underscore (bash variable names), so
# only the libmpv-android pseudo-target differs; it is the published artifact,
# not a component, and is neither fetched nor built by a driver.

# Print a table entry; empty when the component has none. The readers go
# through this because a name built at run time cannot be expanded otherwise
# (the macOS /bin/bash is 3.2, whose indirect expansion does not see a local).
table () {
	eval "printf '%s' \"\${${1//-/_}-}\""
}

components="mbedtls dav1d libdovi ffmpeg freetype fribidi harfbuzz libunibreak libass lua libplacebo mpv"

dep_ffmpeg="mbedtls dav1d libdovi"
dep_libass="freetype fribidi harfbuzz libunibreak"
dep_mpv="ffmpeg libass lua libplacebo"
dep_libmpv_android="mpv"

recipe_mbedtls=mbedtls
recipe_dav1d=meson
recipe_libdovi=libdovi
recipe_ffmpeg=ffmpeg
recipe_freetype=meson
recipe_fribidi=meson
recipe_harfbuzz=meson
recipe_libunibreak=configure
recipe_libass=configure
recipe_lua=lua
recipe_libplacebo=meson
recipe_mpv=meson

args_dav1d="-Denable_tests=false -Db_lto=true -Dstack_alignment=16"
args_fribidi="-Dtests=false -Ddocs=false"
args_harfbuzz="-Dtests=disabled -Ddocs=disabled"
args_libplacebo="-Dvulkan=disabled -Ddemos=false"
# No build date: mpv would stamp __DATE__/__TIME__ into libmpv.so, and a
# rebuild of a published content key has to reproduce its bytes (keys.py
# publish-assets checks). The linux driver passes the same flag.
args_mpv="--default-library shared -Diconv=disabled -Dlua=enabled -Dlibmpv=true -Dcplayer=false -Dmanpage-build=disabled -Dbuild-date=false"

args_libass="--enable-libunibreak --disable-require-system-font-provider"

# What to append to the component's pkg-config Libs: line after install.
pc_libs_libplacebo="-lc++"
