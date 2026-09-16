#!/usr/bin/env bash
# Shared meson recipe: freetype, fribidi, harfbuzz, dav1d, libplacebo and mpv
# all configure the same way, and differ only in the flags depinfo.sh holds.
#
# Usage: meson.sh <build|clean> <component>   (run from deps/<component>)
set -euo pipefail

action=$1
component=${2:?usage: meson.sh <build|clean> <component>}
. ../../include/depinfo.sh
. ../../include/path.sh

build=_build$ndk_suffix

case "$action" in
	clean)
		rm -rf "$build"
		exit 0
		;;
	build) ;;
	*) exit 255 ;;
esac

unset CC CXX # meson wants these unset

meson setup "$build" --cross-file "$prefix_dir"/crossfile.txt $(table "args_$component")
ninja -C "$build" -j$cores
DESTDIR="$prefix_dir" ninja -C "$build" install

# meson does not record the C++ runtime a static library needs
# (https://github.com/mesonbuild/meson/issues/11300), so the components that
# need one name it in the table.
pc_libs=$(table "pc_libs_$component")
if [ -n "$pc_libs" ]; then
	${SED:-sed} "/^Libs:/ s|\$| $pc_libs|" "$prefix_dir/lib/pkgconfig/$component.pc" -i
fi
