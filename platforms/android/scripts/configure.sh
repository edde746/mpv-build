#!/usr/bin/env bash
# Shared autotools recipe: libass and libunibreak both configure out of tree
# and differ only in the flags depinfo.sh holds.
#
# Usage: configure.sh <build|clean> <component>   (run from deps/<component>)
set -euo pipefail

action=$1
component=${2:?usage: configure.sh <build|clean> <component>}
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

# git checkouts (libass) come without configure; release tarballs ship one
[ -f configure ] || ./autogen.sh

mkdir -p "$build"
cd "$build"

../configure --host=$ndk_triple --with-pic \
	--enable-static --disable-shared \
	$(table "args_$component")

make -j$cores
make DESTDIR="$prefix_dir" install
