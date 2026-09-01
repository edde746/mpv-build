# mpv-build

[![mpv](https://img.shields.io/badge/mpv-v0.41.0-blue.svg)](https://github.com/mpv-player/mpv)
[![ffmpeg](https://img.shields.io/badge/ffmpeg-n8.0.1-blue.svg)](https://github.com/FFmpeg/FFmpeg)
[![license](https://img.shields.io/github/license/edde746/mpv-build)](https://github.com/edde746/mpv-build/blob/main/LICENSE)

One repository for every mpv supply chain Plezy ships: the upstream pins, the
patch series, and the build drivers that turn them into `libmpv` binaries for
Apple (XCFrameworks), Android (per-ABI native trees), Linux (prefix bundles)
and Windows (MinGW dev packages), each under `platforms/` or
`Sources/BuildScripts/`.

This repository starts with a clean history. The code it unifies came from:
the Apple driver from [edde746/MPVKit](https://github.com/edde746/MPVKit)
(a fork of [mpvkit/MPVKit](https://github.com/mpvkit/MPVKit), itself derived
from [kingslay/FFmpegKit](https://github.com/kingslay/FFmpegKit)); the Android
driver from [edde746/libmpv-android](https://github.com/edde746/libmpv-android)
(a fork of [jarnedemeulemeester/libmpv-android](https://github.com/jarnedemeulemeester/libmpv-android));
the Linux driver from Plezy's `linux/packaging`; and the Windows driver wraps a
pinned [shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake).
Development history up to the unification lives in those repositories.

## Layout

| Path | Purpose |
| ---- | ------- |
| `versions.json` | Single source of truth for component pins: upstream version, URL and hash/ref per component, with per-platform overrides. |
| `patches/<component>/` | Patch pool plus `series.common` and `series.<platform>` files; line order is the application order. |
| `toolchain/` | Per-group toolchain generation stamps (`toolchain/apple.txt`); bump one to force a full rebuild of that group without a source change. |
| `scripts/` | `patches.py` (series resolution/validation/application), `keys.py` (content-addressed binary keys and the publish gate), and their regression tests. |
| `artifacts.json` | Committed manifest of the published binaries, one section per platform group. |
| `Package.swift` | SwiftPM manifest, rendered from the apple section of `artifacts.json`. |
| `Sources/BuildScripts/` | The Apple build driver (SwiftPM package that compiles and packages the XCFrameworks). |
| `platforms/` | The Android, Linux and Windows build drivers, one `group.json`-defined group per directory. |

## Installation

### Swift Package Manager

```
https://github.com/edde746/mpv-build.git
```

SwiftPM consumers moving from `edde746/MPVKit` change the package URL and pin a
commit of this repository; product names and the Apple build flow are
unchanged. Existing `edde746/MPVKit` pins keep resolving from that repository's
`binaries` release, which stays published.

### License

The packages ship as GPL builds. See [FFmpeg details](https://github.com/FFmpeg/FFmpeg/blob/master/LICENSE.md) and [mpv details](https://github.com/mpv-player/mpv/blob/master/Copyright).

### Pinning a commit

Every push to `main` publishes the binaries that commit needs, so a consumer can
pin any commit and get artifacts built from exactly its sources. In Xcode, add
the package with `Branch/Commit` -> the commit SHA (`kind = revision` in
`project.pbxproj`); semver tags keep working for anyone who wants them.

Binaries are content-addressed: an asset name carries a 12-character key derived
from the component's pins in `versions.json`, its resolved patch series, the
build flags, the platform tuple, the build driver and the toolchain generation.
Assets are therefore immutable, and each platform group publishes to its own
rolling prerelease:
[`binaries-apple`](https://github.com/edde746/mpv-build/releases/tag/binaries-apple),
`binaries-android`, `binaries-linux` and `binaries-windows`. A semver release
is a tag plus notes and carries no assets of its
own. `artifacts.json` records which asset belongs to which library in which
group, and `scripts/keys.py verify` is the gate that keeps every commit on
`main` pinnable:

```bash
# what this working tree needs, and whether it is already published
python3 scripts/keys.py keys --platform-group apple
python3 scripts/keys.py stale --platform-group apple
# fail if the committed manifest or Package.swift do not describe this tree
python3 scripts/keys.py verify --platform-group apple
```

A commit that touches only `patches/mpv/*` moves libmpv's key alone, so CI
compiles libmpv and restores libass and FFmpeg from their published thin
install trees. Editing the build driver moves every key, on purpose:
under-invalidating would ship stale binaries. To force a full rebuild without a
source change -- a new Xcode or SDK, a miscompile -- bump the generation in
`toolchain/apple.txt`.

## Patches

Each patched component keeps its patches in one platform-neutral pool with
per-platform series files:

```
patches/<component>/pool/<name>.patch   the patch bytes
patches/<component>/series.common       applied on every platform, first
patches/<component>/series.<platform>   applied after series.common
```

```bash
# validate every pool and series file
python3 scripts/patches.py check
# the ordered series one platform applies
python3 scripts/patches.py resolve mpv apple
# prove the series still applies to a source tree
python3 scripts/patches.py apply mpv apple <srcdir> --check
```

CI applies every nonempty series against the exact sources `versions.json`
pins, so a version bump that breaks a patch fails before anything builds.

## How to build

```bash
make build
# specified platforms (ios,macos,tvos,tvsimulator,isimulator,maccatalyst,xros,xrsimulator)
make build platform=ios,macos
# clean all build temp files and cache
make clean
# see help
make help
```

## Make demo app using the local build version

If you want the demo app to use the local build version, you need to modify `Package.swift` to reference the local build xcframework file.

<details>
<summary>Click here for more information.</summary>

```
.binaryTarget(
    name: "Libmpv",
    path: "dist/release/Libmpv.xcframework.zip"
),
.binaryTarget(
    name: "Libavcodec",
    path: "dist/release/Libavcodec.xcframework.zip"
),
.binaryTarget(
    name: "Libavdevice",
    path: "dist/release/Libavdevice.xcframework.zip"
),
.binaryTarget(
    name: "Libavformat",
    path: "dist/release/Libavformat.xcframework.zip"
),
.binaryTarget(
    name: "Libavfilter",
    path: "dist/release/Libavfilter.xcframework.zip"
),
.binaryTarget(
    name: "Libavutil",
    path: "dist/release/Libavutil.xcframework.zip"
),
.binaryTarget(
    name: "Libswresample",
    path: "dist/release/Libswresample.xcframework.zip"
),
.binaryTarget(
    name: "Libswscale",
    path: "dist/release/Libswscale.xcframework.zip"
),
```

</details>

## Run default mpv player

```bash
./mpv.sh --input-commands='script-message display-stats-toggle' [url]
./mpv.sh --list-options
```

> Use <kbd>Shift</kbd>+<kbd>i</kbd> to show stats overlay

## Related Projects

* [moltenvk-build](https://github.com/mpvkit/moltenvk-build)
* [libplacebo-build](https://github.com/mpvkit/libplacebo-build)
* [libdovi-build](https://github.com/mpvkit/libdovi-build)
* [libshaderc-build](https://github.com/mpvkit/libshaderc-build)
* [libluajit-build](https://github.com/mpvkit/libluajit-build)
* [libass-build](https://github.com/mpvkit/libass-build)
* [libbluray-build](https://github.com/mpvkit/libbluray-build)

## License

The bundles (`frameworks`, `xcframeworks`), which include both `libmpv` and `FFmpeg` libraries, are licensed under the GPL v3.0.
