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
. ./include/depinfo.sh

# Every pin the run needs, from the resolver the android content key uses
# (keys.py, override-aware). One interpreter for the lot, not one startup per
# field; the function prints shell assignments that pin() reads back, with the
# optional fields versions.json may not carry (ref and commit on archives,
# sha256 on git pins) coming back empty.
pin_block () {
	python3 - "$root" "$components" <<'PY'
import shlex, sys

sys.path.insert(0, sys.argv[1] + "/scripts")
import keys

versions = keys.load_versions(keys.repo_root())
for component in sys.argv[2].split():
    pins = keys.resolved_pins(versions, component, "android")
    kind = keys.resolved_kind(versions, component, "android")
    print(f"pin_{component}_kind={shlex.quote(kind)}")
    print(f"pin_{component}_url={shlex.quote(pins['url'])}")
    for field in ("ref", "sha256", "commit"):
        print(f"pin_{component}_{field}={shlex.quote(pins.get(field, ''))}")
PY
}

# Capture the substitution instead of inlining it: `eval "$( pin_block )"`
# discards the resolver's exit status, because a failed substitution expands to
# an empty command that `eval` reports as success. The run would then die much
# further down, on `<first component>: versions.json pins kind ''`. Check the
# status and name the real failure first.
if ! pins="$( pin_block )"; then
	echo >&2 "download.sh: resolving the android pins from versions.json failed"
	exit 1
fi
eval "$pins"

pin () {
	table "pin_${1}_$2"
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

# Refuse the obsolete prebuilt layout rather than silently retaining the
# vulnerable archive when reusing an existing dependency checkout.
if [ -d deps/libdovi ] && [ ! -f deps/libdovi/dolby_vision/Cargo.toml ]; then
	echo >&2 "deps/libdovi contains obsolete prebuilts; move it aside and rerun download.sh"
	exit 1
fi

mkdir -p deps

# How a component is acquired is a pin (versions.json kind, override-aware), so
# a kind change cannot leave a stale copy of this dispatch behind. Only two
# git pins vendor code in submodules.
for component in $components; do
	case "$(pin "$component" kind)" in
		git)
			case "$component" in
				mbedtls|libplacebo) fetch_git "$component" --recurse-submodules ;;
				*) fetch_git "$component" ;;
			esac
			;;
		archive)
			fetch_archive "$component"
			;;
		*)
			echo >&2 "$component: versions.json pins kind '$(pin "$component" kind)', no fetcher for it"
			exit 1
			;;
	esac
done
