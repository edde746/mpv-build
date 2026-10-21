# mpv-build

[![mpv](https://img.shields.io/badge/mpv-v0.41.0-blue.svg)](https://github.com/mpv-player/mpv)
[![ffmpeg](https://img.shields.io/badge/ffmpeg-n8.0.1-blue.svg)](https://github.com/FFmpeg/FFmpeg)
[![license](https://img.shields.io/github/license/edde746/mpv-build)](https://github.com/edde746/mpv-build/blob/main/LICENSE)

One repository for every mpv supply chain Plezy ships: the upstream pins, the
patch series, and the build drivers that turn them into `libmpv` binaries for
Apple (XCFrameworks), Android (per-ABI native trees and the mpv-android AAR), Linux (prefix bundles)
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
| `scripts/` | `patches.py` (series resolution/validation/application, and `fetch-pinned`), `keys.py` (content-addressed binary keys and the publish gate), `extract.py` (the shared extractor the host regression harnesses slice production code with), and their regression tests. |
| `artifacts.json` | Committed manifest of the published binaries, one section per platform group. |
| `Package.swift` | SwiftPM manifest, rendered from the apple section of `artifacts.json`. |
| `Sources/BuildScripts/` | The Apple build driver (SwiftPM package that compiles and packages the XCFrameworks). |
| `platforms/` | The Android, Linux and Windows build drivers, one `group.json`-defined group per directory; `platforms/android/lib/` is the mpv-android library (JNI glue and Kotlin API) packed into the AAR. |

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

### Debug symbols

The android, linux and windows groups also publish one debug-symbols zip per
variant, so a crash tracker (Sentry or a compatible one) can name the libmpv
and FFmpeg frames of a crash in the shipped binaries. Each zip holds, at its
root, the debug files of the binaries that variant ships. A tracker pairs them
by identity, never by file name: the ELF GNU build-id, or the CodeView GUID
and age a DLL records and its PDB carries.

| Group | Asset, per variant | Contents |
| --- | --- | --- |
| android | `mpv-android-<key>-<abi>-symbols.zip` | a debug ELF for every `.so` in the AAR's `jni/<abi>/`, under the same name (`libc++_shared.so` only when the NDK holds its unstripped copy) |
| linux | `libmpv-linux-<key>-<arch>-symbols.zip` | a debug ELF for every shared library in the tarball |
| windows | `libmpv-windows-<key>-<arch>-symbols.zip` | `libmpv-2.pdb`, the PDB of `libmpv-2.dll` |

The key is the one of the artifact they describe (its `symbolsPattern` in
`group.json`; for the AAR, one zip per ABI of the tarballs it packs). They
upload beside it and are recorded in its `artifacts.json` entry, the checksum
being the zip's sha256:

```json
"libmpv-linux": {
  "key": "<key>",
  "prebuilt": {"x86_64": {"asset": "libmpv-linux-<key>-x86_64.tar.zst", "checksum": "<sha256>"}},
  "symbols": {"x86_64": {"asset": "libmpv-linux-<key>-x86_64-symbols.zip", "checksum": "<sha256>"}}
}
```

`keys.py stale` and `verify` treat an entry without every variant's symbols as
unpublished, and recording a binary whose symbols zip is missing fails.
Packaging never zips debug files by hand: `scripts/symbols.py pack` refuses a
debug file whose identity differs from its shipped binary's or that carries no
symbols, and writes a reproducible zip (sorted members, mode 0644, 2020-01-01
mtimes), because a rebuilt key has to reproduce its published bytes.

```bash
# identities, spelled the way the tracker spells them
python3 scripts/symbols.py id libmpv-2.dll libmpv-2.pdb
# the candidate with <shipped>'s identity and the most debug information
python3 scripts/symbols.py match <shipped> <candidate>...
# a checked, reproducible zip of the debug files
python3 scripts/symbols.py pack out.zip --pair <shipped> <debug> [--pair <shipped> <debug>]...
```

## Patches

Each patched component keeps its patches in one platform-neutral pool with
per-platform series files:

```
patches/<component>/pool/<name>.patch   the patch bytes
patches/<component>/series.common       applied on every platform, first
patches/<component>/series.<platform>   applied after series.common
```

```bash
# validate every pool and series file, and that versions.json's `platforms`
# arrays agree with the groups that actually build each component
python3 scripts/patches.py check
# the ordered series one platform applies
python3 scripts/patches.py resolve mpv apple
# prove the series still applies to a source tree
python3 scripts/patches.py apply mpv apple <srcdir> --check
# acquire the exact source this repository pins and apply its series
python3 scripts/patches.py fetch-pinned mpv apple <srcdir>
```

CI applies every nonempty series against the exact sources `versions.json`
pins, so a version bump that breaks a patch fails before anything builds.

`fetch-pinned` is the one implementation of the pin contract, and the host
regression harnesses below call it instead of each carrying their own copy: it
resolves the override-aware pin, acquires the source the way the pin's `kind`
says -- a `git` pin is shallow-cloned at its `ref` and `HEAD` is proven against
the pinned `commit`; an `archive` pin is downloaded, checked against the pinned
`sha256` and unpacked with its single top directory stripped -- and then applies
the resolved series. The harnesses that build against a source tree also accept
an already-patched source directory as their first argument, which is how they
run offline. The harnesses that slice the patch files directly need no source
tree at all, and `test_patches.sh`, `test_extract.sh`, `test_keys.sh` and
`test_symbols.sh` are regression tests of the Python tools themselves rather
than of a source tree: `patches.py`'s series rules and archive fetching,
`extract.py`'s slicing and hunk parsing over the whole pool, `keys.py`'s
content keys and gates, and `symbols.py`'s identities, matching and zips over
synthetic ELF, PE and PDB files (plus a real linked ELF where `cc`, `readelf`
and `objcopy` link one). `test_apple_merge.sh` (macOS with Xcode) runs the
publish job's XCFramework merge, `scripts/merge_xcframeworks.py`, twice over
the same tiny per-platform frameworks and requires byte-identical zips: a
rebuilt apple key has to reproduce the merged assets already published under
its name.

## How to build

For Apple source builds, select the intended Xcode with `xcode-select` and
install [rustup](https://rustup.rs/) first. Before `make build`, provision the
compiler pinned by this checkout and the targets used by its spatial slices:

```bash
rust_version="$(sed -n 's/^rust=//p' toolchain/apple.txt)"
test -n "$rust_version"
rustup toolchain install "$rust_version" --profile minimal
rustup target add --toolchain "$rust_version" \
  aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios \
  aarch64-apple-tvos aarch64-apple-tvos-sim
```

These are the same toolchain and target prerequisites installed by Apple CI.
Repeat this setup after changing checkouts if their `rust` pin differs.

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

A rebuild of a published content key has to reproduce the published tarballs
byte for byte, so no Android library may record when it was built: mpv is
configured with `-Dbuild-date=false`, and OpenSSL, whose "built on" date ffmpeg
links into `libavformat.so`, builds with `SOURCE_DATE_EPOCH` fixed to the
tarballs' 2020-01-01 mtime. Every build leg of every group (and the Apple
merge and the AAR build) runs `bash scripts/test_build_date.sh <asset>...`
over its assets, debug-symbols zips included, before uploading. It unpacks
each `.tar.gz`, `.tar.zst`, `.zip` or `.aar`
(and the `.jar` inside) and fails when a file carries a
clock reading from any day since 2020-01-02: `__DATE__`, asctime, ISO 8601 or
RFC 2822 text, a PE or COFF `TimeDateStamp`, an ar member mtime, a Mach-O dylib
timestamp, or an archive entry's own mtime. The range reaches back that far
because a restored Windows build tree keeps the stamps of the runs that
compiled each package. `--day YYYY-MM-DD` checks an older asset against the
day it was built.

Run the host-side MediaCodec timing regression with
`bash scripts/test_mediacodec_timing.sh` (Bash, Python 3, and a C compiler).
It applies the full Android series to pinned mpv and extracts the production
timing and prepare/draw/flip paths. Coverage includes cadence prediction,
refresh hysteresis, clock drift across the mp_time/CLOCK_MONOTONIC boundary,
playback-speed changes, seek during preparation, dropped-frame still redraws
without resetting cadence, the invariant the codec submission lead exists
for: every frame reaches MediaCodec 50 ms (media3's early-scheduling
threshold) before the presentation timestamp it is given, not before its raw
deadline; releases snapped with the Choreographer's own vsync spacing when it
disagrees with the display mode (an Amlogic box's SurfaceFlinger runs 15.71 ms
against a 16.68 ms mode), so every release stays on the grid SurfaceFlinger
latches against across resamples and changes in that spacing; Choreographer
samples that step off the grid for one resample (a MediaTek projector's, 0.4
to 0.5 of a vsync every few seconds) never move a release, while a step a
later resample confirms is followed one resample late and a steady drift off
the snap period (a Pixel 7's 11.057 ms panel under its 11.111 ms mode) is
followed every resample; the sparse statistics cadence (a line when a failure
counter moves, on the first tick, and as a 60 s heartbeat otherwise), and the
video-plane presentation feedback: a release records the vsync it aimed at,
the codec's rendered report is matched back by presentation time and bucketed
by vsync error, and a reset forgets intents but keeps the counters. Pass an
already-patched source directory as the first argument to run offline.

Run the vsync sampler regression with `bash scripts/test_mediacodec_vsync.sh`.
It applies the full Android series and extracts vo_mediacodec's
process-lifetime Choreographer sampler whole (its callbacks, its thread, and
the attach, detach and fresh-sample requests the VO makes), then drives it
against a model of Android 11's AChoreographer as first released
(android-11.0.0_r1, which Fire OS 8 behaves like). That release reports a
display mode switch twice, through DisplayManager and through SurfaceFlinger's
config-changed event, and delivers the second from inside a frame callback
posted for the next vsync on the Choreographer's own thread. Coverage: no
Choreographer entry point is ever called with the sampler's lock held, a
switch delivered from inside the first callback's post (and one already
queued when the thread starts) keeps the last period reported and tells the
vo about each, every reservation reaches the Choreographer and returns to one
500 ms resample, the API 33 timeline resample and a VO request join it, and a
detached vo is never sampled. Pass an already-patched source directory as the
first argument to run offline.

Run the OSD scheduler regression with `bash scripts/test_mediacodec_osd.sh`.
It applies the full Android series and extracts the freestanding scheduler
core and request dispatcher of the shared subtitle render-ahead pipeline
(`video/out/osd_ahead.[ch]`, patch 0031), plus vo_mediacodec's swap lead;
pass an already-patched source directory to run offline. Coverage includes
frame-cadence prediction, next-frame pre-render matching, the subtitle read
horizon, event warming and staging lifetime, and
swap lead. Request scenarios ensure that a warmed image cannot replace an
expired cue's blank frame, a newer or overlapping cue, a timestamp-matched
pre-render, or a repaint after seeking.
Completed animation poses use an immutable bounded FIFO rather than replacing
each other while the presenter waits. Queue regressions cover ordered handoff,
full-capacity admission, wraparound, release ownership, and epoch cancellation.
The same pipeline renders subtitles for vo_avfoundation (patch 0004, which
carries all of `vo_avfoundation.m`); series.common lists 0031 with the sub/
patches it needs (0205 render threads, 0011, 0012, 0013 and 0014), so every
group builds it. The request scenarios also cover the helpers a presenter on
the video thread uses: a request counts as served even when the OSD did not
change, only the newest flipped result is drawn, and every queued result, and
nothing else, notifies the presenter (vo_avfoundation redraws the paused frame
with a result that missed it). `bash scripts/test_avf_osd_ahead.sh` applies
the Apple series and checks how long flip_page may hold a frame's video for
its subtitles: until half the queue lead is left, not at all while the render
thread is behind, a bounded time for untimed frames (redraws, and frames shown
while paused), and as long as before in the composite modes.

Run the OSD surface-retirement regression with
`bash scripts/test_mediacodec_rebind.sh`. It applies the full Android series
and exercises live OSD replacement without rebuilding the video decoder:
retirement waits for the old producer, releases its window exactly once, and
starts the replacement without stale requests or timing state. An
already-patched source directory may be passed for offline runs.

Live OSD replacement requires the matching JNI surface-generation handoff,
which ships in the same commit's mpv-android AAR (`platforms/android/lib`); a
timeout quarantines a live producer rather than releasing resources it may
still use.

Run the libass series regressions with `bash scripts/test_libass_series.sh`.
It fetches the pinned upstream libass, applies `patches/libass/series.common`
(the threaded renderer, layout cache and fast blur that used to be the
edde746/libass fork, and the shape cache), builds it for the
host (freetype, fribidi, harfbuzz and libunibreak via pkg-config) and checks
that a static frame is reported unchanged on repeat and after handing off a
prefetched renderer while another renderer has advanced the same track.
Two-way handoffs also release only event-layout snapshots while both renderers
remain alive and reusable. Retained frame pixels survive cache release and
renderer reuse; overlapping-event renders settle unchanged on a same-timestamp
repeat, and clearing the old owner's cache again leaves the new owner's frame
unchanged. Cue expiry clears both renderers without preserving stale pixels.
mpv's subtitle packer relies on that unchanged result to reuse its prepared
atlas. The shape-cache harness renders cues whose shaped runs differ from an
earlier cue in one input (font size, font, kerning) through
one renderer and requires each frame to match a renderer that has shaped nothing
else. Pass an already-patched source directory as the first argument to run
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

Run the AVFoundation Dolby Vision profile 5 regression with
`bash scripts/test_avf_dovi_profile5_rpu.sh`. It extracts the selector from
patch 0004 that decides which software-decoded frames vo_avfoundation uploads
full range with their RPU attached, so the display layer reshapes profile 5 on
devices without a Dolby Vision decoder (plezy#2760). It checks that only a
profile 5 frame (RPU profile 0, full-range base layer) presented as Dolby Vision
is picked, in any side-data order, and that profile 8, other RPU profile and
range combinations, non-Dolby Vision frames and frames missing their metadata
or RPU keep their old upload.

Run the MediaCodec input regression with `bash scripts/test_mediacodec_input.sh`.
It fetches the pinned ffmpeg revision, applies the Android series, and
exercises the production input-buffer sizing and packet submission: an access
unit that does not fit its input buffer is dropped whole and the decoder
flushed, never split across buffers. Pass an already-patched source directory
as the first argument to run offline.

Run the MediaCodec end-of-stream regression with `bash scripts/test_mediacodec_eos.sh`.
It fetches the pinned ffmpeg revision, applies the Android series, and drives
the asynchronous decoder's drain through the production send and receive:
after a flush has advanced the timestamp generation, an EOS the codec reports
with a timestamp of its own (0, -1) still ends the drain and its buffer goes
back unrendered, an EOS flagged on an echoed last frame delivers the frame
first, output finished before the flush is still dropped without pre-empting
a drain that has not started, and the polled decoder reads the flag alone.
Pass an already-patched source directory as the first argument to run offline.

Run the MediaCodec flush regression with `bash scripts/test_mediacodec_flush.sh`.
It fetches the pinned ffmpeg revision, applies the Android series, and drives
the production flush entry against the polled and the asynchronous decoder:
a seek on a codec that has handed out no input (the one a resume position
makes right after opening the decoder) leaves it unflushed and keeps the
asynchronous codec's offered slots and timestamps, a seek after input flushes
it, back-to-back seeks flush once, and an oversized first access unit still
gives its slot back through a flush. Pass an already-patched source directory
as the first argument to run offline.

Run the MediaCodec codec-error regression with `bash scripts/test_mediacodec_error.sh`.
It fetches the pinned ffmpeg revision, applies the Android series, and drives
the production receive_frame callback, the codec callbacks and the flush
against a fake codec, on the asynchronous and the polled decoder: an error
the codec reported before a seek, a flush the codec failed and a restart it
failed each stay the decoder's answer on every later call, across later
seeks too, so mpv's hardware-decoding fallback runs; a seek without an error
decodes on. Pass an already-patched source directory as the first argument
to run offline.

Run the paused MediaCodec regression with `bash scripts/test_mediacodec_paused.sh`.
It fetches the pinned mpv and ffmpeg revisions, applies their Android series,
and drives mpv's decoder filter step and wakeup registration over FFmpeg's
production receive_frame callback, codec callbacks, dequeues, busy answer and
buffer releases, against a fake codec on a virtual clock, with a player that
holds the codec's output buffers mid-file and in a drain at the end of the
file. In both codec modes the paused decoder is not asked at all from 2 s on
until the player gives a buffer back (rendered at a time, rendered, or
dropped), a run for any other reason asks once, every call holds the decoding
thread at most 10 ms (mpv's seeks, property reads, window changes and teardown
wait on that thread), no error line is logged, and a resumed decoder picks up
the codec's next frame within one 8 ms poll. A synchronous codec still
decoding its last frames, falling behind with no buffer held, or taking input
without handing out a frame is asked until its frame comes; a paused seek
decodes at once. Pass an already-patched mpv tree and an FFmpeg tree as the
first two arguments to run offline.

Run the MediaCodec output-crop regression with `bash scripts/test_mediacodec_crop.sh`.
It exercises the production FFmpeg format parser: MediaTek's configured
placeholder crop is ignored, but decoded output crops remove buffer padding
and follow resolution changes. Java crop keys and legacy fallback dimensions
retain their precedence. Pass an already-patched source directory to run offline.

Run the rendered-frame feedback regression with
`bash scripts/test_mediacodec_rendered.sh`. It fetches the pinned ffmpeg
revision, applies the Android series, and exercises the production ring the
codec's frame-rendered callback fills and `av_mediacodec_drain_rendered`
empties: delivery order across bounded drains, a full ring dropping and
counting the newest report, a producer racing a consumer across many wraps,
and a codec without feedback answering ENOSYS. The VO side - each release's
intended vsync matched to the codec's report, the vsync histogram and the
`video-present` line, which names its source and on an `off:*` source
records no intents and keeps only the submission-side `late`/`unsnapped`
counters live - is covered by `test_mediacodec_timing.sh`. Pass an
already-patched source directory as the first argument to run offline.

Run the MediaCodec `media3_rate` regression with
`bash scripts/test_mediacodec_rate.sh`. It applies the Android series to the
pinned ffmpeg and exercises the production path. The option defaults to auto:
on for decoders named `c2.mtk.`, `OMX.MTK.` or `OMX.MS.`, off for the rest and
for a decoder that could not be named, and an explicit value wins either way.
With the option on, the decoder sets the operating rate to the stream's frame
rate, rounded, once, after its first frame, a refused rate or a missing frame
rate ends the attempt without costing the frame, and the host's
`av_mediacodec_set_operating_rate` never reaches the codec; without it the
host's rate does and frames set nothing. The keys the option leaves out of
the configure format are logged by the decoder, not covered here. Pass an
already-patched source directory as the first argument to run offline.

Run the pre-decode frame shedding regression with
`bash scripts/test_mediacodec_shed.sh`. It fetches the pinned ffmpeg
revision, applies the Android series, does a minimal host configure of the
tree (the AV1 path needs FFmpeg's coded-bitstream reader) and exercises the
production classifiers a MediaCodec decoder runs when the host asks for
`AVDISCARD_NONREF`: H.264 by `nal_ref_idc`, HEVC by sub-layer non-reference
type at the highest temporal sub-layer, VP9 and AV1 by `refresh_frame_flags`,
with a VP9 superframe's hidden frames kept and the packet truncated in place,
and an AV1 temporal unit shed only when the shed frame is all it carries (a
unit that also holds a hidden frame has to keep the frame it shows). The AV1
corpus is a real SVT-AV1 stream (`--regenerate` re-encodes it with a host
ffmpeg); the reader is the oracle for what every unit holds, and a stream the
reader keeps refusing switches shedding off rather than logging per unit.
Pass an already-patched source directory as the first argument to run
offline.

Run the presentation-order regression with
`bash scripts/test_mediacodec_reorder.sh`. It compiles the production
recovery (`libavcodec/mediacodec_reorder.c`, patch 0026) for the host and
drives it with a model decoder that outputs frames in display order by the
HEVC bumping process and echoes each frame's queued timestamp, as MediaCodec
does, over x265-shaped streams (a four-frame B pyramid, open-GOP CRA anchors
with RASL leading pictures). A container that stores B-frames without
composition offsets queues decode-order timestamps: once the output steps
backwards the frames are presented at the queued timestamps in increasing
order, and every recovered frame lands on its display time. Seeks to a CRA
keep that timeline: the RASL pictures a decoder skips are taken out when
queued, or by the reorder bound when the access units are not visible, and a
decoder that outputs them anyway has them counted back in. A frame the decoder
drops is written off within the reorder bound; long sessions with seeks keep
the bookkeeping bounded. Well-formed B-frame timestamps, a discontinuity, a
missing timestamp and a stream the decoder never reorders all leave the
codec's timestamps untouched. Pass an already-patched source directory as the
first argument to run offline.

Run the PGS palette regression with `bash scripts/test_pgs_palette.sh`. It
compiles pgssubdec's palette cache -- `find_palette`, `flush_cache` and the
palette segment parser -- and drives an epoch boundary across it: a palette
segment inside one epoch still updates, inheriting the entries it omits, while
a palette slot re-issued after the epoch flush starts fully transparent instead
of keeping the previous epoch's colours (the stale entry that drew an opaque
box behind PGS subtitles). Pass an already-patched source directory to run
offline.

Run the TCP hostname-lookup regression with
`bash scripts/test_tcp_family_lookup.sh`. It compiles tcp.c's per-family
lookup (patch 0029) under ASan and UBSan against a scripted resolver and
connection race on a virtual clock: time moves only when every thread waits,
so each scripted delay is exact and a loaded machine cannot move an answer
across the 50 ms Resolution Delay. A family whose lookup never answers --
the AAAA query a broken resolver drops -- holds nothing up: the race starts
with the other family 50 ms after it answered, a family that answers later
joins a race stuck on a blackholed address or is raced alone once every
earlier address failed, IPv6 is listed first, a failed lookup never ends the
open while the other is pending, and the caller's interrupt ends any wait
without being taken for a late answer. Every lookup the open left behind
frees what it shares, and literals keep the single AF_UNSPEC lookup. Pass an
already-patched source directory to run offline.

Run the codec-profile regression with `bash scripts/test_demux_codec_profile.sh`.
It extracts the profile the demuxers publish when a file loads (patch
0204: demux_mkv names it from CodecPrivate, demux_lavf from MP4's avcC or
hvcC), which is what a client's per-file decode routing reads before any
decoder exists, and compares it with the profile mpv publishes once the pinned
FFmpeg's software decoder has decoded the stream's first access unit (a
minimal host configure builds the H.264 and HEVC decoders). The samples are
one-frame x264 and x265 encodes muxed into Matroska and MP4: HEVC Main, Main 10
and Rext, the same records rewritten to a general_profile_idc of 0 that names
the profile only in the compatibility flags, and H.264 Constrained Baseline,
High, High 10 and High 10 Intra. A record whose configurationVersion is not 1,
one shorter than its fixed header, Annex B extradata or another codec names no
profile. `--regenerate` re-encodes the samples with a host ffmpeg (libx264 and
libx265). Pass an already-patched mpv tree and an FFmpeg tree as the first two
arguments to run offline.

Run the OSD-plane letterbox regression with `bash scripts/test_mediacodec_letterbox.sh`.
It compiles the production fill against a recording GL stub: the fill covers
exactly the render's margins in surface pixels, rounds a scaled plane outward,
paints nothing for cover/zoom or a full-frame picture, and leaves the scissor
and clear color as the subtitle draw expects. Pass an already-patched source
directory to run offline.

Run the decoder busy-policy regression with `bash scripts/test_lavc_busy_policy.sh`.
It compiles mpv's `lavc_process` against scripted send/receive callbacks and
a recording filter graph: a decoder that wakes the filter itself (MediaCodec,
either codec mode) has a refused packet put back without a retry and is not
asked again past the EOF packet until it wakes the filter, then delivers its
last frames and a single EOF; an ordinary libavcodec decoder refusing a packet
gets a warning and an immediate retry and is left alone past EOF; neither is
spun while the demuxer starves it. Pass an already-patched source directory
to run offline.

Run the frame-queue regression with `bash scripts/test_player_queue_frames.sh`.
It compiles the core's frame-queue gate and frame-duration code
(`player/video.c`) against a scripted decoder: `vo_mediacodec`, which times
each frame from its own pts, gets a decoded frame without waiting for the next
one while the decoder is still asked for that next frame; a frame queued alone
lasts the spacing measured from the previous frame's pts (20 ms for a decoder
splitting a 25 fps interlaced stream into fields), which also settles the
previous frame's guessed duration, while a timestamp jump stays with the frame
before it; and other VOs, the first frame after a reset, display-sync and EOF
keep upstream behavior. Pass an already-patched source directory to run
offline.

Run the audio-reload regression with `bash scripts/test_audio_reload_held_frame.sh`.
It compiles `reload_audio_output()` and the ao filter's `ao_process()`
(`player/audio.c`, patch 0034) against pins that behave like `filters/filter.c`:
a reload while the ao filter still holds a frame meant for the old AO releases
that frame once, wakes the ao filter, and leaves the output chain asked for data,
so the next AO opens; a reload with nothing held keeps the chain asked and drops
nothing. Pass an already-patched source directory to run offline.

Run the held-frame release regression with `bash scripts/test_player_release_held_frame.sh`.
It compiles `uninit_video_chain()` (`player/video.c`) and `vo_release_frames()`
(`video/out/vo.c`, patch 0139) against a decoder whose frames share its
reference count, as FFmpeg's MediaCodec context does: ending a file under
`vo_mediacodec` leaves no frame holding the decoder, which is deleted on the VO
thread with the VO lock released, so a single-instance decoder (MediaTek
MT5895) is free when the next file opens one; a GL VO keeps its last frame for
redraws; no video chain touches nothing. Pass an already-patched source
directory to run offline.

Run the refresh-seek regression with `bash scripts/test_demux_refresh.sh`. It
compiles `add_packet_locked()` and `clear_queue()` (`demux/demux.c`, patch
0208) and replays what a refresh seek reads back, set up as the cache-join
resume and a track switch set it up: a stream that was already selected drops
exactly the packets at or before its old position, so video reading its old
tail back queues nothing twice, and keeps the first packet past it, which for a
subtitle stream with no packet between the seek target and its old position is
its next line. Streams with DTS and position-only streams are both covered.
Pass an already-patched source directory to run offline.

Run the spatial audio output regression with `bash scripts/test_ao_spatial.sh`
(macOS). It drives the driver with a real synchronizer and a fake sink:
admission and the bytes waiting for a full sink, the clock's start, holds and
resumes, pause, silence-only input, gaps, empty and gapless EOF, and the
per-player fallback latch. `bash scripts/test_spatial_feed.sh` compiles the
production Swift bridge with SpatialAudioBridge's pinned passthrough sink and
IEC unpacker and a fake renderer: unpacking, progress and queue coverage, the
end, new timelines, gaps, failures, volume and close. Neither test opens an
audio device or establishes receiver behavior. CI runs both on pull requests,
and compiles the bridge for tvOS against the pinned SpatialAudioBridge.

`ao_spatial` hands mpv's IEC 61937 bursts to SpatialAudioBridge's
`SpatialAudioPassthroughSink`, which unpacks, converts and queues APAC on the
output's synchronizer. The driver owns that synchronizer's rate. It reads the
sink's progress (how far enqueued audio reaches, and whether the end of the
stream is enqueued) and the renderer's queue, which also misses audio a route
change flushed until it is enqueued again. The clock starts once the sink is
ready, stops with 250 ms of that audio left, and resumes with 500 ms queued, or
with 250 ms when the sink takes no more, which may mean either a full renderer
or a conversion that is behind. A slow conversion therefore holds playback
instead of letting the renderer drop late audio; a finished stream plays out to
its end once the renderer holds all of it, and drains, stopping the clock, when
the clock passes it. mpv's ao thread checks the output every 125 ms while it streams. These
are scheduling policies, not timing or queue-capacity guarantees.

What the sink cannot take yet, about half a second, waits in the output and
counts against `free_samples`; the sink itself buffers a second ahead of the
renderer. Silence converts to nothing, so the clock idles at the end of the
converted audio without failing. The output opts out of
`audio-stream-silence`: it feeds no receiver, and with streamed silence mpv
would write zeros through a pause instead of pausing it. An IEC 61937 pause
burst or format change starts a new timeline; any other sink failure latches
the format and reloads the output for mpv's PCM decode path.

The output is self-contained: `scripts/build_spatial.py` compiles the
SpatialAudioBridge source commit in `versions.json`, with `cargo --locked` and
the Rust version in `toolchain/apple.txt`. Its Rust core and Swift sink are
folded into `Libmpv`; that source pin and the build script are part of
libmpv's content key, and of no other library's. There is no application
callback or separate SpatialAudioBridge package. The binary includes the
silent Atmos clip and its license notices. Like mpv's other Apple outputs, it
sets the audio session's category, mode and multichannel support and activates
it when it opens, and leaves it to the host afterwards.

The Rust core is built as a staticlib alone with fat LTO, then prelinked into
one object. Following LibTrueHD's packaging approach, only `sab_*` and a weak
`rust_eh_personality` remain global. This avoids collisions with libdovi's Rust
runtime while preserving decoder panic handling. The build checks the exported
symbols and weak personality for every supported slice.
After each Apple platform build that compiles libmpv, the publish workflow runs
`bash scripts/test_spatial_archives.sh <patched-mpv> <platform>`, which checks the installed slices and links their sink with force-loaded libdovi,
without media. It also links an Objective-C-only Clang consumer using the
packaged `mpv.pc`, including a toolchain path containing spaces. The prelink
log records the Mach-O platform and minimum OS.
The SwiftPM wrapper contains Swift source so C/Objective-C consumers also get
the Swift runtime and back-deployment libraries. After packaging,
`bash scripts/test_spatial_swiftpm.sh <release-dir>` links an Objective-C-only
app on iOS and tvOS against the packaged frameworks (requires XcodeGen).

Static pkg-config consumers must configure the install paths in
`pkgconfig-example` and select their Xcode toolchain. `mpv.pc` supplies both
Swift runtime and platform-specific compatibility-library flags; its distributed
`swift_toolchain` value is a placeholder, not the build machine's Xcode path.
Set that variable in the installed file or override it when invoking pkg-config:

```bash
swift_compiler="$(xcrun --find swiftc)"
swift_toolchain="$(dirname "$(dirname "$(dirname "$swift_compiler")")")"
pkg-config --define-variable="swift_toolchain=$swift_toolchain" --static --libs mpv
```

Use the metadata for the target slice: device and simulator Swift libraries
live in different toolchain directories. The selected Xcode must be compatible
with the compiler used to build the binaries.

Spatial playback requires OS 26 and a supported Rust target: arm64 tvOS and
its simulator, or iOS and its simulators. macOS, Mac Catalyst, arm64e and Intel
tvOS retain their other audio outputs. mpv's macOS build includes LuaJIT, and
LuaJIT, C++, Objective-C and the core's Rust would require four unwind
personality routines, exceeding the binary's compact-unwind limit of three.
Ordinary TrueHD/DTS-HD and conversion failures fall back to PCM on the spatial
path. Source inspection confirms that a tvOS consumer using `ao=spatial,`
also reaches PCM when the spatial driver is absent:
the trailing comma enables ordinary output probing; AudioUnit rejects every
SPDIF format (`audio/out/ao_audiounit.m:116`), and AVFoundation accepts only
AC-3/E-AC-3 (`audio/out/ao_avfoundation.m:1180`). With no output accepting
TrueHD/DTS-HD passthrough, `player/audio.c:467` disables SPDIF and reinitializes
the decoder. These are lines in the patched mpv tree. This path can log output
probe refusals; it does not return the playback error below the PCM retry.
The unsupported slices were inspected, not run.

macOS needs a different guard: CoreAudio redirects non-PCM to
`coreaudio_exclusive` (`audio/out/ao_coreaudio.c:162`), which accepts SPDIF
(`audio/out/ao_coreaudio_exclusive.c:295`) and may retain that format for a
compressed device stream (`:356`). Plezy never requests passthrough on macOS:
its macOS player selects `coreaudio,avfoundation`
(`macos/Runner/MpvPlayer/MpvPlayerCore.swift:125`), and
`PlatformDetector.supportsAudioPassthrough()` excludes macOS
(`lib/utils/platform_detector.dart:312`), gating `audio-spdif` in
`lib/screens/video_player_screen.dart:1981`. This is source inspection of
Plezy's configuration, not a guarantee that arbitrary mpv passthrough settings
fall back to PCM.

For a real muted renderer test, build
the macOS dependency with `build_spatial.py`, then run
`bash scripts/test_spatial_renderer.sh <patched-mpv> <built-spatial-dir> <IEC-file> <codec> <expect-failure>`.
Codec is 1 for TrueHD or 2 for DTS-HD; the final argument is 0 or 1. Supply
IEC 61937 generated by FFmpeg's `spdif` muxer, with `-dtshd_rate 768000` for DTS-HD.
An optional final libdovi archive argument force-loads its Rust runtime too,
so the same test checks that the prelinked core coexists with it.

Run the AudioTrack deadline regression with
`bash scripts/test_audiotrack_timing.sh`. It fetches the pinned mpv revision,
applies the Android series, and exercises the production clock and audio-buffer
deadline code with deterministic JNI delays. Coverage includes passthrough,
PCM timestamp/fallback paths, startup, E-AC3 counter wrap, partial writes
across stop/reset/recreation, stale write completions without clock credit,
DTS-HD bursts unwrapping to bare packets with the raw track reopened at
the core rate, and the passthrough clock's rate-limited `playhead step`
diagnostic with its observe-only `getTimestamp` probe, and a stalled raw
TrueHD track demoting TrueHD to decoding instead of being recreated. Pass an
already-patched source directory as the first argument to run offline.

Run the raw TrueHD regression with `bash scripts/test_audiotrack_truehd.sh`.
It encodes TrueHD with the host ffmpeg, packs it into IEC 61937 MAT bursts
with ffmpeg's spdif muxer, and runs the bursts through the production
reassembly: every access unit handed to the raw track must be byte-identical
to the unit ffmpeg packed, in whole groups of 24, credited one unit at a time,
whether the carrier is read in whole bursts or split mid-word. Streams cover
small units in wide padding, units larger than the MAT slot (so the codes
split units and units cross frames), a pickup mid-burst that must resume on a
major sync, a damaged MAT code, and a 44.1 kHz stream that must demote.
Pass an already-patched source directory as the first argument to run
offline, and a media file as the second to check its first audio track too.

### Android library (mpv-android AAR)

`platforms/android/lib/` is the Android library both apps play through: the
JNI glue `libplayer.so` (C++, `src/main/cpp`) and its Kotlin API in package
`com.edde746.mpv` (`src/main/kotlin`). It lives here because it depends on this
repository's patches (mpv's `vo-mediacodec-osd-surface`,
`vo-mediacodec-video-rect` and `vo-format-rejected`, FFmpeg's
`av_jni_set_mediacodec_callback_class`), so it is built and versioned with
them. The AAR is self-contained: per ABI (`arm64-v8a`, `armeabi-v7a`, `x86`,
`x86_64`) it carries `libplayer.so`, `libmpv.so`, the seven FFmpeg libraries
and `libc++_shared.so`, plus `classes.jar` and the consumer R8 rules that keep
every member the JNI code reaches. It needs nothing at run time beyond the
Kotlin standard library; minSdk 25. The glue derives in part from
libmpv-android (MIT); `platforms/android/lib/NOTICE` carries its notice.

The API, one `MpvPlayer` per native session:

```kotlin
val player = MpvPlayer.create(context, listener, hookTimeoutMillis = 3_000) {
    setOption("vo", "gpu")            // pre-init options; create() sets none of its own
}
player.observeProperty("pause", PropertyFormat.Flag)
player.command("loadfile", url)       // the playlist entry id, or null
player.attachSurfaces(videoSurface, osdSurface, videoGeneration, osdGeneration)
player.close()
```

- Every call blocks; run them off the Android main thread (create,
  attachSurfaces, observeProperty, setLogLevel and close refuse it).
- `MpvPlayer.Listener` receives `onPropertyChange`, `onEvent` (log lines arrive
  as `MpvEvent.LogMessage` in the same stream, so a line explaining a failed
  open comes before its end-file) and `onHook`, on one library-owned thread per
  session, in mpv's order. mpv's event thread only enqueues: it never blocks
  and nothing is dropped.
- mpv holds playback at a hook (`on_preloaded`) until `continueHook(id)`;
  with `hookTimeoutMillis > 0` the library continues it itself that long after
  `onHook` was called, and a throwing `onHook` is continued at once.
- `close()` waits for the native teardown; once it starts no listener call
  starts, and every unanswered hook is continued.

`mpv-android` is a package artifact of the android group (`group.json`): its
key covers the libmpv-android key, every file under `platforms/android/lib`
that git tracks and `scripts/symbols.py`, so editing the glue moves the AAR's
key alone, and anything that moves the tarballs' key moves it too. It publishes to
`binaries-android` as `mpv-android-<key>.aar`, with one
`mpv-android-<key>-<abi>-symbols.zip` per ABI (see
[Debug symbols](#debug-symbols)), and is recorded in `artifacts.json` beside
the tarballs:

```json
"mpv-android": {
  "key": "<key>", "asset": "mpv-android-<key>.aar", "checksum": "<sha256>",
  "symbols": {
    "arm64-v8a": {"asset": "mpv-android-<key>-arm64-v8a-symbols.zip", "checksum": "<sha256>"},
    "armeabi-v7a": {"asset": "mpv-android-<key>-armeabi-v7a-symbols.zip", "checksum": "<sha256>"},
    "x86": {"asset": "mpv-android-<key>-x86-symbols.zip", "checksum": "<sha256>"},
    "x86_64": {"asset": "mpv-android-<key>-x86_64-symbols.zip", "checksum": "<sha256>"}
  }
}
```

```bash
python3 scripts/keys.py keys --platform-group android   # libmpv-android and mpv-android
```

An app resolves it with Gradle through an Ivy repository on the release URL,
the key as the version, no authentication:

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        ivy {
            url = uri("https://github.com/edde746/mpv-build/releases/download")
            patternLayout { artifact("binaries-android/[artifact]-[revision].[ext]") }
            metadataSources { artifact() }
            content { includeModule("edde746", "mpv-android") }
        }
    }
}
// build.gradle.kts
dependencies { implementation("edde746:mpv-android:<key>@aar") }
```

The sha256 from `artifacts.json` goes into the app's pin (its
`mpv-build.lock.json`) and Gradle's dependency verification.

Publishing: `.github/workflows/android.yml` builds the four tarballs (when
libmpv-android is stale) and then the AAR (`platforms/android/lib/build-aar.sh`,
with the JVM tests), from that run's tarballs or the published ones
`artifacts.json` pins. `publish.yml` runs it for main, records the AAR and its
symbols zips with `keys.py record-packages`, verifies and uploads them with
the tarballs; `ci.yml` runs it for pull requests (tarballs only when the
change moves their key) and also builds the device tests' APK. The AAR's
`.so` files compile against the
tarball's `include/`, which carries mpv's headers and the patched FFmpeg's
(`libavcodec/jni.h` among them): no header is vendored.

Building it locally needs JDK 17+ and an Android SDK (`ANDROID_HOME`); Gradle
installs the NDK `toolchain/android.txt` pins:

```bash
# the tarballs this checkout's artifacts.json pins (fetched, sha256-checked)
bash platforms/android/lib/build-aar.sh
# any published tarball key, or tarballs built here (package.sh)
bash platforms/android/lib/build-aar.sh --key <libmpv-android key>
bash platforms/android/lib/build-aar.sh --tarballs dist/release
```

The result is `dist/release/mpv-android.aar` and, beside it, one
`dist/release/mpv-android-<abi>-symbols.zip` per ABI: the debug file of every
`.so` the AAR ships (cut from the tarball's unstripped copy, Gradle's
`libplayer.so` or the NDK's `libc++_shared.so` with `llvm-objcopy
--only-keep-debug`), which needs `python3`, `zip` and `unzip`. A library of
ours without a GNU build-id or a debug file fails the build, so tarballs
published before the android libraries were linked with `--build-id=sha1`
(every libmpv-android key published before then, `f16b821414ff` included) no
longer build an AAR. To try it in an app before it is
published, copy it to a directory as `mpv-android-<name>.aar` and point the
app's Ivy repository at that directory (`url = uri("file:///that/dir")`,
pattern `[artifact]-[revision].[ext]`) with `<name>` as the version.

Tests: `bash scripts/test_android_glue.sh` builds and runs the glue's host
C++ tests (`src/test/cpp`: session lifecycle, event order with the error
end-file drain, UTF-8 conversion) against fakes, with the patched mpv and
FFmpeg headers; `./gradlew testDebugUnitTest` (in `platforms/android/lib`;
AGP 9 runs a library's unit tests against its debug build type only)
runs the JVM tests of the listener delivery and hook rules; the device test
(`src/androidTest`) creates a player, plays the media given as the
instrumentation argument `mediaUrl`, attaches and replaces surfaces and
destroys it:

```bash
./gradlew -Pmpv.tarballs=<dir> assembleDebugAndroidTest
adb install -r -t build/outputs/apk/androidTest/debug/mpv-android-debug-androidTest.apk
adb shell am instrument -w -e mediaUrl <url> com.edde746.mpv.test/androidx.test.runner.AndroidJUnitRunner
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
