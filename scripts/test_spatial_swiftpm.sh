#!/usr/bin/env bash
# Link an Objective-C-only host through the actual SwiftPM product. Unpublished
# XCFrameworks come from the release directory; unchanged dependencies use their
# pinned URLs. No consumer Swift files, runtime flags, signing or app execution.
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
if not (frameworks / 'Libmpv.xcframework').is_dir():
    raise SystemExit('Expected freshly packaged Libmpv.xcframework')

def local_target(match):
    name = match['name']
    if not (frameworks / f'{name}.xcframework').is_dir():
        return match[0]
    return f'.binaryTarget(name: "{name}", path: "Frameworks/{name}.xcframework")'

text = re.sub(r'\.binaryTarget\(\s*name: "(?P<name>[^"\n]+)",\s*'
              r'url: "[^"\n]+",\s*checksum: "[^"\n]+"\s*\)',
              local_target, manifest.read_text())
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
    dependencies:
      - package: MPVKit
        product: MPVKit
YAML
xcodegen generate --spec "$work/project.yml"
for platform in iOS tvOS; do
    xcodebuild -project "$work/SpatialConsumer.xcodeproj" -scheme "Consumer-$platform" \
        -configuration Release -destination "generic/platform=$platform" \
        -derivedDataPath "$work/DerivedData" CODE_SIGNING_ALLOWED=NO build
    case "$platform" in iOS) sdk=iphoneos ;; tvOS) sdk=appletvos ;; esac
    binary="$work/DerivedData/Build/Products/Release-$sdk/Consumer-$platform.app/Consumer-$platform"
    xcrun nm -gUj "$binary" > "$work/symbols.txt"
    grep -q '^_mp_spatial_create$' "$work/symbols.txt"
    xcrun otool -L "$binary" > "$work/libraries.txt"
    grep 'libswiftCore.dylib' "$work/libraries.txt"
    echo "$platform: Objective-C-only SwiftPM consumer linked the spatial sink and Swift runtime"
done
