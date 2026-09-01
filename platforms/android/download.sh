#!/usr/bin/env bash
# Fetches every android source component at the exact pin versions.json
# resolves for the android group (base pins with overrides.android folded in)
# and applies each component's android patch series via scripts/patches.py.
#
# Idempotent per component: an existing deps/<name> is left alone (delete it
# to re-fetch and re-patch). Git pins are verified against the recorded commit
# when versions.json carries one; archives are verified against their pinned
# sha256. The host SDK/NDK bootstrap lives in include/download-sdk.sh.
set -euo pipefail

cd "$( dirname "${BASH_SOURCE[0]}" )"
root="$( cd ../.. && pwd )"

pin () {
	python3 - "$root/versions.json" "$1" "$2" <<'PY'
import json, sys
entry = json.load(open(sys.argv[1]))["components"][sys.argv[2]]
override = (entry.get("overrides") or {}).get("android") or {}
print(override.get(sys.argv[3], entry.get(sys.argv[3], "")))
PY
}

apply_patches () {
	( cd "$root" && python3 scripts/patches.py apply "$1" android "platforms/android/deps/$1" )
}

fetch_git () {
	local component=$1
	shift
	if [ -d "deps/$component" ]; then
		return 0
	fi
	local url ref commit head
	url=$(pin "$component" url)
	ref=$(pin "$component" ref)
	commit=$(pin "$component" commit)
	git clone --depth 1 --branch "$ref" "$@" "$url" "deps/$component"
	if [ -n "$commit" ]; then
		head=$(git -C "deps/$component" rev-parse HEAD)
		if [ "$head" != "$commit" ]; then
			echo >&2 "$component: $ref is $head, versions.json pins $commit"
			exit 1
		fi
	fi
	apply_patches "$component"
}

fetch_archive () {
	local component=$1
	if [ -d "deps/$component" ]; then
		return 0
	fi
	local url sha archive digest
	url=$(pin "$component" url)
	sha=$(pin "$component" sha256)
	archive="deps/.$component.archive"
	curl -fsSL -o "$archive" "$url"
	digest=$(python3 -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$archive")
	if [ -z "$sha" ]; then
		echo >&2 "$component: warning: versions.json pins no sha256 (downloaded $digest)"
	elif [ "$digest" != "$sha" ]; then
		echo >&2 "$component: archive sha256 $digest, versions.json pins $sha"
		exit 1
	fi
	mkdir -p "deps/$component"
	tar -xzf "$archive" --strip-components=1 -C "deps/$component"
	rm -f "$archive"
	apply_patches "$component"
}

# libdovi ships as prebuilt per-Rust-triple static libraries; the pinned URL
# is a template with {triple}. The C API header is vendored at
# include/libdovi/rpu_parser.h because the release tarballs carry only the
# archive.
fetch_libdovi () {
	if [ -d deps/libdovi ]; then
		return 0
	fi
	local template triple
	template=$(pin libdovi url)
	for triple in aarch64-linux-android armv7-linux-androideabi i686-linux-android x86_64-linux-android; do
		mkdir -p "deps/libdovi/$triple"
		curl -fsSL "${template//\{triple\}/$triple}" | tar -xz -C "deps/libdovi/$triple"
	done
}

mkdir -p deps

fetch_git mbedtls --recurse-submodules
fetch_git dav1d
fetch_libdovi
fetch_git ffmpeg
fetch_git freetype
fetch_git fribidi
fetch_git harfbuzz
fetch_archive libunibreak
fetch_git libass
fetch_archive lua
fetch_git libplacebo --recurse-submodules
fetch_git mpv
