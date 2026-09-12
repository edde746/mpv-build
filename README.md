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

### Android

Install [rustup](https://rustup.rs) as well as the Android host tools and pinned
NDK (`bash platforms/android/include/download-sdk.sh`). Then build from the
repository root:

```bash
bash platforms/android/download.sh
bash platforms/android/build.sh --arch arm64
```

Other architectures are `armv7l`, `x86`, and `x86_64`. Android now builds libdovi
from the pinned source and lockfile, including the malformed-RPU bounds fix;
the build installs the Rust compiler pinned in `toolchain/android.txt` and the
selected Android target.
The old per-target libdovi prebuilts must not be reused.

`download.sh` leaves existing dependency directories untouched. After changing
patches or source pins, move the old `platforms/android/deps` and
`platforms/android/prefix` directories aside before fetching and rebuilding.
An obsolete prebuilt-only `deps/libdovi` is explicitly rejected. Use a fresh
build before packaging; changing patch files does not update installed binaries.

Run the host-side MediaCodec timing regression with
`bash scripts/test_mediacodec_timing.sh` (Bash, Python 3, and a C compiler).
It applies the full Android series to pinned mpv and extracts the production
timing and prepare/draw/flip paths. Coverage includes cadence prediction,
refresh hysteresis, clock drift across the mp_time/CLOCK_MONOTONIC boundary,
playback-speed changes, seek during preparation, dropped-frame still redraws
without resetting cadence, and the invariant the codec submission lead exists
for: every frame reaches MediaCodec at least two display periods before the
presentation timestamp it is given, not before its raw deadline. Pass an
already-patched source directory as the first argument to run offline.

Run the OSD scheduler regression with `bash scripts/test_mediacodec_osd.sh`.
It extracts the freestanding scheduler core and request dispatcher of the
same patch. Coverage includes frame-cadence prediction, next-frame pre-render
matching, the subtitle read horizon, event warming and staging lifetime, and
swap lead. Request scenarios ensure that a warmed image cannot replace an
expired cue's blank frame, a newer or overlapping cue, a timestamp-matched
pre-render, or a repaint after seeking.
Completed animation poses use an immutable bounded FIFO rather than replacing
each other while the presenter waits. Queue regressions cover ordered handoff,
full-capacity admission, wraparound, release ownership, and epoch cancellation.

Run the OSD surface-retirement regression with
`bash scripts/test_mediacodec_rebind.sh`. It applies the full Android series
and exercises live OSD replacement without rebuilding the video decoder:
retirement waits for the old producer, releases its window exactly once, and
starts the replacement without stale requests or timing state. An
already-patched source directory may be passed for offline runs.

Live OSD replacement requires the matching Plezy JNI surface-generation
handoff. Update the application and Android libmpv artifacts together; a
timeout quarantines a live producer rather than releasing resources it may
still use.

Run the libass change-detection regression with
`bash scripts/test_libass_change_detection.sh`. It fetches the pinned upstream
libass, applies `patches/libass/series.common` (the threaded renderer, layout
cache and fast blur that used to be the edde746/libass fork), builds it for the
host (freetype, fribidi, harfbuzz and libunibreak via pkg-config) and checks
that a static frame is reported unchanged on repeat and after handing off a
prefetched renderer while another renderer has advanced the same track.
Two-way handoffs also release only event-layout snapshots while both renderers
remain alive and reusable. Retained frame pixels survive cache release and
renderer reuse; overlapping-event renders settle unchanged on a same-timestamp
repeat, and clearing the old owner's cache again leaves the new owner's frame
unchanged. Cue expiry clears both renderers without preserving stale pixels.
mpv's subtitle packer relies on that unchanged result to reuse its prepared
atlas. Pass an already-patched source directory as the first argument to run
offline.

`scripts/test_libass_rounding.c` checks the ARMv7 VFP rounding fast path against
the device's `lrint`: ties, signed limits, non-finite values, subnormals, all four
rounding modes, and preserved exception state. Cross-compile it with the Android
ARMv7 compiler, `-O2 -I<patched-libass>/libass -lm`, then run the executable on
an ARMv7-capable Android device. It deliberately rejects a host-only build,
which would exercise the unchanged fallback instead of the VFP instructions.

Run the Dolby Vision packet-filter regression with
`bash scripts/test_mediacodec_dv_filter.sh`. It extracts the production
access-unit filter from the ffmpeg patch and checks that unchanged access units
are neither allocated nor copied, that HDR10+ SEI messages and profile 7
layers are removed or converted as configured, that malformed SEI is left
verbatim, and that the output buffer is reused.

Run the MediaCodec input regression with `bash scripts/test_mediacodec_input.sh`.
It fetches the pinned ffmpeg revision, applies the Android series, and
exercises the production input-buffer sizing and packet submission: an access
unit that does not fit its input buffer is dropped whole and the decoder
flushed, never split across buffers. Pass an already-patched source directory
as the first argument to run offline.

Run the MediaCodec output-crop regression with `bash scripts/test_mediacodec_crop.sh`.
It exercises the production FFmpeg format parser: MediaTek's configured
placeholder crop is ignored, but decoded output crops remove buffer padding
and follow resolution changes. Java crop keys and legacy fallback dimensions
retain their precedence. Pass an already-patched source directory to run offline.

Run the OSD-plane letterbox regression with `bash scripts/test_mediacodec_letterbox.sh`.
It compiles the production fill against a recording GL stub: the fill covers
exactly the render's margins in surface pixels, rounds a scaled plane outward,
paints nothing for cover/zoom or a full-frame picture, and leaves the scissor
and clear color as the subtitle draw expects. Pass an already-patched source
directory to run offline.

Run the AudioTrack deadline regression with
`bash scripts/test_audiotrack_timing.sh`. It fetches the pinned mpv revision,
applies the Android series, and exercises the production clock and audio-buffer
deadline code with deterministic JNI delays. Coverage includes passthrough,
PCM timestamp/fallback paths, startup, E-AC3 counter wrap, partial writes
across stop/reset/recreation, and stale write completions without clock credit.
Pass an already-patched source directory as the first argument to run offline.

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
