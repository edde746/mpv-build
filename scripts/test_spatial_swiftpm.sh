#!/usr/bin/env bash
# Link an Objective-C-only host through the actual SwiftPM product. XCFrameworks
# this run packaged come from the release directory and the rest use their
# pinned URLs, so a run that rebuilt only FFmpeg links it against the pinned
# libmpv, as a consumer would. No consumer Swift files, runtime flags, signing
# or app execution.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release_dir="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/MPVKit/Sources" "$work/Consumer"
cp -R "$root/Sources/_MPVKit" "$root/Sources/_FFmpeg" "$work/MPVKit/Sources/"
python3 - "$root/Package.swift" "$release_dir" "$work/MPVKit" <<'PY'
from pathlib import Path
import re
import sys
import zipfile

manifest, release, package = map(Path, sys.argv[1:])
frameworks = package / 'Frameworks'
frameworks.mkdir()
for archive in sorted(release.glob('*.xcframework.zip')):
    with zipfile.ZipFile(archive) as zipped:
        zipped.extractall(frameworks)
fresh = {path.name[:-len('.xcframework')] for path in frameworks.glob('*.xcframework')}
if not fresh:
    raise SystemExit(f'Expected at least one freshly packaged XCFramework in {release}')

linked = set()

def local_target(match):
    name = match['name']
    if name not in fresh:
        return match[0]
    linked.add(name)
    return f'.binaryTarget(name: "{name}", path: "Frameworks/{name}.xcframework")'

text = re.sub(r'\.binaryTarget\(\s*name: "(?P<name>[^"\n]+)",\s*'
              r'url: "[^"\n]+",\s*checksum: "[^"\n]+"\s*\)',
              local_target, manifest.read_text())
if fresh - linked:
    raise SystemExit('Freshly packaged but not a binary target of Package.swift: ' + ', '.join(sorted(fresh - linked)))
(package / 'Package.swift').write_text(text)
PY
cat > "$work/Consumer/main.m" <<'OBJC'
@import Foundation;
@import Libmpv;
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
cat > "$work/project.yml" <<'YAML'
name: SpatialConsumer
packages:
  MPVKit:
    path: MPVKit
settings:
  base:
    CODE_SIGNING_ALLOWED: NO
    GENERATE_INFOPLIST_FILE: YES
    CLANG_ENABLE_MODULES: YES
    ARCHS: arm64
targets:
  Consumer-iOS:
    type: application
    platform: iOS
    deploymentTarget: '15.0'
    sources: [Consumer]
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: test.mpv.spatial.ios
      LD_GENERATE_MAP_FILE: YES
      LD_MAP_FILE_PATH: '$(SRCROOT)/$(TARGET_NAME).map'
    dependencies:
      - package: MPVKit
        product: MPVKit
  Consumer-tvOS:
    type: application
    platform: tvOS
    deploymentTarget: '15.0'
    sources: [Consumer]
    settings:
      PRODUCT_BUNDLE_IDENTIFIER: test.mpv.spatial.tvos
      LD_GENERATE_MAP_FILE: YES
      LD_MAP_FILE_PATH: '$(SRCROOT)/$(TARGET_NAME).map'
    dependencies:
      - package: MPVKit
        product: MPVKit
YAML
xcodegen generate --spec "$work/project.yml"
for platform in iOS tvOS; do
    link_map="$work/Consumer-$platform.map"
    xcodebuild -project "$work/SpatialConsumer.xcodeproj" -scheme "Consumer-$platform" \
        -configuration Release -destination "generic/platform=$platform" \
        -derivedDataPath "$work/DerivedData" CODE_SIGNING_ALLOWED=NO build
    case "$platform" in iOS) sdk=iphoneos ;; tvOS) sdk=appletvos ;; esac
    binary="$work/DerivedData/Build/Products/Release-$sdk/Consumer-$platform.app/Consumer-$platform"
    # The private Swift entry point need not stay globally visible in a Release
    # executable. Require it in the linker's live symbols, excluding dead code.
    python3 - "$link_map" "$platform" <<'PY'
from pathlib import Path
import sys

# Link maps can contain non-UTF-8 symbol names; the required markers are ASCII.
text = Path(sys.argv[1]).read_bytes()
live = text.split(b'# Symbols:\n', 1)[1].split(b'# Dead Stripped Symbols:', 1)[0]
matches = [line for line in live.splitlines()
           if line.split() and line.split()[-1] == b'_mp_spatial_create']
if not matches:
    raise SystemExit(f'{sys.argv[2]}: spatial entry point missing from live link map')
print(f'{sys.argv[2]}: live spatial entry point: {matches[0].decode("ascii", errors="backslashreplace")}')
PY
    xcrun nm -gUj "$binary" > "$work/global-symbols.txt"
    if grep -q '^_mp_spatial_create$' "$work/global-symbols.txt"; then
        echo "$platform: spatial entry point remains global"
    else
        xcrun nm -Uj "$binary" > "$work/all-symbols.txt"
        if grep -q '^_mp_spatial_create$' "$work/all-symbols.txt"; then
            echo "$platform: spatial entry point is local, excluded by nm -g"
        else
            echo "$platform: symbol table omits the private entry point; live link map verified it"
        fi
    fi
    xcrun otool -L "$binary" > "$work/libraries.txt"
    if ! grep 'libswiftCore.dylib' "$work/libraries.txt"; then
        cat "$work/libraries.txt"
        echo "$platform: missing Swift runtime dependency" >&2
        exit 1
    fi
    echo "$platform: Objective-C-only SwiftPM consumer linked the spatial sink and Swift runtime"
done
