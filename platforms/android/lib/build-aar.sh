#!/usr/bin/env bash
# Builds the mpv-android AAR (README.md, Android library) from four libmpv-android tarballs and leaves it as
# dist/release/mpv-android.aar, the unkeyed name `keys.py record-packages` renames to the group.json
# assetPattern's content-addressed one.
#
# Usage: build-aar.sh [--tarballs <dir> | --key <key>] [-- <more Gradle tasks or flags>]
#   (none)           the tarballs artifacts.json pins at this checkout, fetched from binaries-android and checked
#                    against the recorded sha256 (fetch-tarballs.py)
#   --key <key>      the published tarballs of any libmpv-android content key, checked against the sha256 GitHub
#                    records for each asset
#   --tarballs <dir> local tarballs, e.g. dist/release after package.sh (libmpv-android-<abi>.tar.gz) or a
#                    directory of published ones (libmpv-android-<key>-<abi>.tar.gz)
#   -- <args>        passed to Gradle after assembleRelease (CI adds testDebugUnitTest assembleDebugAndroidTest)
# Needs JDK 17 or newer and an Android SDK (ANDROID_HOME, or sdk.dir in platforms/android/lib/local.properties);
# Gradle installs the NDK toolchain/android.txt pins and CMake 4.1.2 when the SDK lacks them.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"

tarballs=
key=
extra=()
while [ $# -gt 0 ]; do
	case "$1" in
		--tarballs) shift; tarballs=$1 ;;
		--key) shift; key=$1 ;;
		--) shift; extra=("$@"); break ;;
		*) sed -n '2,15p' "$0" >&2; exit 2 ;;
	esac
	shift
done
if [ -n "$tarballs" ] && [ -n "$key" ]; then
	echo "build-aar.sh: --tarballs and --key are exclusive" >&2
	exit 2
fi

if [ -z "$tarballs" ]; then
	tarballs="$(python3 "$here/fetch-tarballs.py" ${key:+--key "$key"})"
fi
tarballs="$(cd "$tarballs" && pwd)"

# ${extra[@]+...}: macOS's bash 3.2 calls an empty array unbound under set -u.
(cd "$here" && ./gradlew --stacktrace "-Pmpv.tarballs=$tarballs" assembleRelease ${extra[@]+"${extra[@]}"})

mkdir -p "$root/dist/release"
cp "$here/build/outputs/aar/mpv-android-release.aar" "$root/dist/release/mpv-android.aar"
echo "built dist/release/mpv-android.aar from $tarballs"
