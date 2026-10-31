# Apple

The apple group publishes one XCFramework zip per library to
[`binaries-apple`](https://github.com/edde746/mpv-build/releases/tag/binaries-apple).
`Package.swift` is rendered from the `platforms.apple` section of
`artifacts.json` and exposes them as the `MPVKit` product. The build driver is
the SwiftPM package in `Sources/BuildScripts/` (`make build`); the publish job
merges the per-platform legs with `scripts/merge_xcframeworks.py`.

Libraries this repository does not compile come from the prebuilt releases
`versions.json` pins (mostly `mpvkit/*-build`).

## Testing a local build in the Demo apps

The Demo projects (`Demo/Demo.xcworkspace`) reference this package by local
path. To run them against a local `make build`, point the binary targets in
`Package.swift` at the zips it wrote, one per framework:

```swift
.binaryTarget(name: "Libmpv", path: "dist/release/Libmpv.xcframework.zip"),
.binaryTarget(name: "Libavcodec", path: "dist/release/Libavcodec.xcframework.zip"),
// ... Libavdevice, Libavformat, Libavfilter, Libavutil, Libswresample, Libswscale
```

Do not commit that edit: `keys.py verify` requires `Package.swift` to match
`artifacts.json`.

## Static pkg-config consumers

Configure the install paths in `pkgconfig-example` and select an Xcode
compatible with the compiler that built the binaries. `mpv.pc` adds the Swift
runtime and compatibility-library flags, but its `swift_toolchain` value is a
placeholder. Set it in the installed file or on the command line:

```bash
swift_compiler="$(xcrun --find swiftc)"
swift_toolchain="$(dirname "$(dirname "$(dirname "$swift_compiler")")")"
pkg-config --define-variable="swift_toolchain=$swift_toolchain" --static --libs mpv
```

Use the metadata of the target slice: device and simulator Swift libraries
live in different toolchain directories. The SwiftPM wrapper contains Swift
source for the same reason, so C and Objective-C consumers also get the Swift
runtime and back-deployment libraries.

## AVPlayer passthrough output

`ao_avfoundation`'s compressed (AVPlayer) sink needs a host that services the
main dispatch queue: AVPlayer attaches its item on the main thread. A host
whose main thread never does (a command-line client blocked in
`mpv_wait_event()`) gets no resource-loader call; the output seeks to the start
after about 2 s and falls back to PCM after about 4.4 s, and both log lines say
AVPlayer made no resource requests. Run the main loop on the main thread
(`NSApplication`, `RunLoop.main.run()` or `dispatchMain()`) and drive libmpv
from another thread.

A passthrough output that cannot play its stream after opening (this sink, or
`ao_spatial`'s renderer) calls `ao_request_pcm_fallback()`. The player decodes
that track to PCM and seeks back to the playback position. The fallback
belongs to the track: the next track or file, or a later output reload, tries
passthrough again. Patches 0040-0042 carry the details.

## Spatial audio (`ao=spatial`)

`ao_spatial` (patch 0039) plays TrueHD Atmos and DTS:X by handing mpv's IEC
61937 bursts to SpatialAudioBridge's `SpatialAudioPassthroughSink`, which
unpacks, converts and queues APAC on the output's synchronizer.

- **Where it exists.** OS 26 on arm64 tvOS and its simulator, and on iOS and
  its simulators. macOS, Mac Catalyst, arm64e and Intel tvOS keep their other
  audio outputs: macOS links LuaJIT, and LuaJIT, C++, Objective-C and the
  core's Rust would need four unwind personality routines, one more than the
  compact-unwind format allows.
- **Fallback.** TrueHD/DTS-HD and conversion failures fall back to PCM. With
  `ao=spatial,` on a slice without the driver, the trailing comma enables
  ordinary output probing; no other Apple output takes TrueHD/DTS-HD
  passthrough, so mpv disables SPDIF and decodes to PCM. That path was checked
  by reading the source, not run. On macOS, CoreAudio hands non-PCM to
  `coreaudio_exclusive`, which accepts SPDIF, so a macOS host must not request
  TrueHD/DTS-HD passthrough itself.
- **Clock.** The driver owns the synchronizer's rate. The clock starts once the
  sink is ready, stops with 250 ms of converted audio left, and resumes with
  500 ms queued, or 250 ms when the sink takes no more. A slow conversion
  holds playback instead of letting the renderer drop late audio. These are
  scheduling policies, not timing guarantees.
- **Buffering.** About half a second the sink cannot take yet waits in the
  output and counts against `free_samples`; the sink buffers a second ahead of
  the renderer. Silence converts to nothing, so the output opts out of
  `audio-stream-silence`. An IEC 61937 pause burst or format change starts a
  new timeline; any other sink failure requests the PCM fallback.
- **Audio session.** Like mpv's other Apple outputs, it sets the session's
  category, mode and multichannel support and activates it when it opens, then
  leaves it to the host.

### How it is built

`scripts/build_spatial.py` compiles the SpatialAudioBridge commit pinned in
`versions.json` with `cargo --locked` and the Rust version in
`toolchain/apple.txt`. The Rust core and Swift sink are folded into `Libmpv`;
that pin and `build_spatial.py` are part of libmpv's content key only. There is
no separate SpatialAudioBridge package or application callback. The binary
includes the silent Atmos clip and its licence notices.

The Rust core is built as a fat-LTO staticlib and prelinked into one object in
which only `sab_*` and a weak `rust_eh_personality` stay global, so it does not
collide with libdovi's Rust runtime. The build checks those exports for every
slice.

### Its tests

| Script | What it checks | Runs in |
| --- | --- | --- |
| `scripts/test_ao_spatial.sh` | The driver against a real synchronizer and a fake sink: admission, clock start/hold/resume, pause, silence, gaps, EOF, the PCM fallback. | `ci.yml`, `publish.yml` |
| `scripts/test_spatial_feed.sh` | The production Swift bridge with the pinned sink and IEC unpacker against a fake renderer. | `ci.yml`, `publish.yml` |
| `scripts/test_spatial_archives.sh <patched-mpv> <platform>` | The installed slices: exports, a link with force-loaded libdovi, an Objective-C consumer through `mpv.pc`. | `publish.yml`, after each platform build |
| `scripts/test_spatial_swiftpm.sh <release-dir>` | An Objective-C-only app on iOS and tvOS linked through the packaged SwiftPM product (needs XcodeGen). | `publish.yml`, after packaging |
| `scripts/test_spatial_renderer.sh <patched-mpv> <built-spatial-dir> <iec-file> <1=truehd\|2=dtshd> <expect-failure:0\|1> [libdovi-archive]` | A real, muted renderer on macOS. Make the IEC file with FFmpeg's `spdif` muxer (`-dtshd_rate 768000` for DTS-HD). | manual |

None of these opens an audio device or establishes receiver behaviour.
