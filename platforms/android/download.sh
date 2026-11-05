#!/usr/bin/env bash
# Fetches every android source component at the exact pin versions.json
# resolves for the android group (base pins with overrides.android folded in)
# and applies each component's android patch series, through
# `scripts/patches.py fetch-pinned`: the one implementation of the pin
# contract. A git pin's HEAD has to equal its pinned commit and an archive has
# to match its pinned sha256. Each source is tried three times, then the pin's
# `mirror` when it has one, and the mirror answers to the same pin.
#
# Idempotent per component: an existing deps/<name> is left alone (delete it
# to re-fetch and re-patch). The host SDK/NDK bootstrap lives in
# include/download-sdk.sh.
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
root="$( cd ../.. && pwd )"
. ./include/depinfo.sh

# Refuse the obsolete prebuilt layout rather than silently retaining the
# vulnerable archive when reusing an existing dependency checkout.
if [ -d deps/libdovi ] && [ ! -f deps/libdovi/dolby_vision/Cargo.toml ]; then
	echo >&2 "deps/libdovi contains obsolete prebuilts; move it aside and rerun download.sh"
	exit 1
fi

mkdir -p deps

for component in $components; do
	if [ -d "deps/$component" ]; then
		continue
	fi
	( cd "$root" && python3 scripts/patches.py fetch-pinned "$component" android "platforms/android/deps/$component" )
	# Only libplacebo vendors code in submodules, at the commits its pinned
	# tree names; its series patches none of them.
	if [ "$component" = libplacebo ]; then
		git -C "deps/$component" submodule update --init --recursive
	fi
done
