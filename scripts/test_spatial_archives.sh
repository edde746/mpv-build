#!/usr/bin/env bash
# Check installed slices and Swift/pkg-config consumer links with libdovi, without media.
# Usage: test_spatial_archives.sh <patched-mpv> <platform>
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="$1"
platform="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/consumer.m" <<'OBJC'
#import <Foundation/Foundation.h>
#include <mpv/client.h>
int main(void) {
    @autoreleasepool {
        mpv_handle *player = mpv_create();
        if (!player) return 1;
        int result = mpv_initialize(player);
        mpv_terminate_destroy(player);
        return result < 0;
    }
}
OBJC
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
            xcrun --sdk "$sdk" clang -target "$target" -isysroot "$sdk_path" -fobjc-arc -I"$source_dir" \
                -c "$root/scripts/test_spatial_renderer.m" -o "$work/test.o"
            xcrun --sdk "$sdk" swiftc -target "$target" -sdk "$sdk_path" "$work/test.o" "$archive" \
                -Xlinker -force_load -Xlinker "$root/dist/libdovi/$platform/thin/$arch/lib/libdovi.a" \
                -framework AVFoundation -framework Foundation -framework CoreMedia \
                -framework AudioToolbox -framework Accelerate -o "$work/test"
            echo "$platform/$arch: combined libmpv and force-loaded libdovi linked"

            # Use the distributed metadata, including its configurable toolchain
            # root. No Swift search paths may be supplied directly to Clang.
            packaged_pc="$root/dist/release/libmpv/pkgconfig-example/$platform/$arch/mpv.pc"
            grep -Fxq 'swift_toolchain=/path/to/Swift.xctoolchain' "$packaged_pc"
            mkdir -p "$work/pkgconfig"
            python3 - "$packaged_pc" "$work/pkgconfig" "$root/dist" "$sdk_path" <<'PY'
from pathlib import Path
import re
import sys
source, destination, prefix, sdk = sys.argv[1:]
destination, sdk = Path(destination), Path(sdk)
(destination / 'mpv.pc').write_text(Path(source).read_text().replace('/path/to/workdir', prefix))
# Apple SDKs supply these system libraries without .pc files. Describe only
# the selected SDK, never Homebrew's host libraries or macOS header paths.
for name, library, header, macro, include in [
    ('zlib', 'z', 'zlib.h', 'ZLIB_VERSION', ''),
    ('libxml-2.0', 'xml2', 'libxml2/libxml/xmlversion.h', 'LIBXML_DOTTED_VERSION', 'libxml2'),
]:
    stub = sdk / f'usr/lib/lib{library}.tbd'
    if not stub.is_file():
        raise SystemExit(f'Missing target SDK library: {stub}')
    text = (sdk / 'usr/include' / header).read_text()
    version = re.search(r'^#\s*define\s+' + macro + r'\s+"([^"]+)"', text, re.MULTILINE)
    if not version:
        raise SystemExit(f'Missing {macro} in target SDK header {header}')
    (destination / f'{name}.pc').write_text(
        f'Name: {name}\nDescription: Selected Apple SDK system library\n'
        f'Version: {version[1]}\nLibs: -l{library}\n'
        f'Cflags: -I"{sdk / "usr/include" / include}"\n')
    print(f'{name} {version[1]}: {stub}', flush=True)
PY
            swift_compiler="$(xcrun --find swiftc)"
            swift_toolchain="$(dirname "$(dirname "$(dirname "$swift_compiler")")")"
            ln -sfn "$swift_toolchain" "$work/Selected Xcode.xctoolchain"
            pc_dirs="$work/pkgconfig"
            for directory in "$root/dist/"*/"$platform/thin/$arch/lib/pkgconfig"; do
                pc_dirs+=":$directory"
            done
            PKG_CONFIG_LIBDIR="$pc_dirs" PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= \
                PKG_CONFIG_ALLOW_SYSTEM_LIBS=1 pkg-config \
                --define-variable="swift_toolchain=$work/Selected Xcode.xctoolchain" \
                --static --cflags --libs mpv > "$work/consumer-flags.txt"
            python3 - "$target" "$sdk_path" "$work" \
                "$root/dist/libdovi/$platform/thin/$arch/lib/libdovi.a" <<'PY'
from pathlib import Path
import shlex
import subprocess
import sys
target, sdk, temporary, libdovi = sys.argv[1:]
work = Path(temporary)
flags = shlex.split((work / 'consumer-flags.txt').read_text())
print(f'{target}: emitted pkg-config flags: {shlex.join(flags)}', flush=True)
subprocess.run(['xcrun', '--sdk', sdk, 'clang', '-target', target, '-isysroot', sdk, '-fobjc-arc',
                str(work / 'consumer.m'), '-Xlinker', '-force_load', '-Xlinker', libdovi,
                *flags, '-o', str(work / 'consumer')], check=True)
PY
            xcrun nm -Uj "$work/consumer" > "$work/consumer-symbols.txt"
            grep -q '^_mp_spatial_create$' "$work/consumer-symbols.txt"
            xcrun otool -L "$work/consumer" > "$work/consumer-libraries.txt"
            grep 'libswiftCore.dylib' "$work/consumer-libraries.txt"
            echo "$platform/$arch: Clang consumer linked with packaged pkg-config flags and force-loaded libdovi"
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
