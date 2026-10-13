#!/usr/bin/env bash
# Check the installed libmpv slices and link their sink with libdovi, without media.
# Usage: test_spatial_archives.sh <patched-mpv> <platform>
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$1"
platform="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
shopt -s nullglob
archives=("$root/dist/libmpv/$platform/thin/"*/lib/libmpv.a)
if (( ${#archives[@]} == 0 )); then
    echo "No installed libmpv archives for $platform" >&2
    exit 1
fi
for archive in "${archives[@]}"; do
    thin="$(dirname "$(dirname "$archive")")"
    arch="$(basename "$thin")"
    xcrun nm -gUj "$archive" > "$work/exports.txt"
    case "$platform:$arch" in
        ios:arm64|isimulator:arm64|isimulator:x86_64|tvos:arm64|tvsimulator:arm64)
            grep -q '^_mp_spatial_create$' "$work/exports.txt"
            grep -q '^_sab_' "$work/exports.txt"
            xcrun nm -m "$archive" > "$work/symbols.txt"
            grep -E 'weak external (\[no dead strip\] )?_rust_eh_personality$' "$work/symbols.txt"
            case "$platform" in
                ios) sdk=iphoneos; target="$arch-apple-ios14.0" ;;
                isimulator) sdk=iphonesimulator; target="$arch-apple-ios14.0-simulator" ;;
                tvos) sdk=appletvos; target="$arch-apple-tvos14.0" ;;
                tvsimulator) sdk=appletvsimulator; target="$arch-apple-tvos14.0-simulator" ;;
            esac
            sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
            xcrun clang -target "$target" -isysroot "$sdk_path" -fobjc-arc -I"$source_dir" \
                -c "$root/scripts/test_spatial_renderer.m" -o "$work/test.o"
            xcrun swiftc -target "$target" -sdk "$sdk_path" "$work/test.o" "$archive" \
                -Xlinker -force_load -Xlinker "$root/dist/libdovi/$platform/thin/$arch/lib/libdovi.a" \
                -framework AVFoundation -framework Foundation -framework CoreMedia \
                -framework AudioToolbox -framework Accelerate -o "$work/test"
            echo "$platform/$arch: combined libmpv and force-loaded libdovi linked"
            ;;
        *)
            if grep -Eq '^_(sab_|mp_spatial_)' "$work/exports.txt"; then
                echo "$platform/$arch unexpectedly includes the spatial sink" >&2
                exit 1
            fi
            echo "$platform/$arch: no spatial sink or core symbols"
            ;;
    esac
done
