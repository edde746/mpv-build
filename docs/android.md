# Android

The android group publishes two things to
[`binaries-android`](https://github.com/edde746/mpv-build/releases/tag/binaries-android):

- `libmpv-android-<key>-<abi>.tar.gz`, one per ABI (`arm64-v8a`,
  `armeabi-v7a`, `x86`, `x86_64`): libmpv, FFmpeg and their dependencies,
  built for API 25 with the NDK pinned in `toolchain/android.txt`.
- `mpv-android-<key>.aar`: the Android library both apps play through. It packs
  the four tarballs' native libraries with the JNI glue and its Kotlin API.

Both are defined in `platforms/android/group.json` and recorded under
`platforms.android` in `artifacts.json`.

## Native tarballs

### Building

Needs [rustup](https://rustup.rs) (libdovi is built from source) and the host
tools and NDK that `download-sdk.sh` installs. From the repository root:

```bash
bash platforms/android/include/download-sdk.sh
bash platforms/android/download.sh
bash platforms/android/build.sh --arch arm64     # armv7l, arm64, x86, x86_64; default all four
bash platforms/android/package.sh --arch arm64   # -> dist/release/libmpv-android-arm64-v8a.tar.gz
```

- libdovi comes from the pinned dovi_tool source, built with `cargo --locked`
  and the Rust version in `toolchain/android.txt`;
  `platforms/android/scripts/libdovi.sh` installs that toolchain and the ABI's
  target.
- `download.sh` leaves an existing `platforms/android/deps/<name>` alone, so
  after changing patches or pins, move `platforms/android/deps` and
  `platforms/android/prefix` aside and fetch and build again. It refuses a
  `deps/libdovi` that holds prebuilts instead of source.
- No library may record when it was built, because a rebuilt key has to
  reproduce its published bytes: mpv is configured with `-Dbuild-date=false`
  and OpenSSL builds with `SOURCE_DATE_EPOCH` fixed at 2020-01-01.
  `scripts/test_build_date.sh` checks every asset before upload.

## mpv-android AAR

`platforms/android/lib/` holds the JNI glue `libplayer.so` (C++,
`src/main/cpp`) and its Kotlin API in package `com.edde746.mpv`
(`src/main/kotlin`). It lives here because it depends on this repository's
patches: mpv's `vo-mediacodec-osd-surface`, `vo-mediacodec-video-rect` and
`vo-format-rejected`, and FFmpeg's `av_jni_set_mediacodec_callback_class`.

The AAR is self-contained. Per ABI it carries `libplayer.so`, `libmpv.so`, the
seven FFmpeg libraries and `libc++_shared.so`, plus `classes.jar` and consumer
R8 rules that keep every member the JNI code reaches. At run time it needs only
the Kotlin standard library. minSdk is 25. The glue derives in part from
libmpv-android (MIT) and the playback helpers from Plezy (GPL-3.0);
`platforms/android/lib/NOTICE` carries both notices.

### API

One `MpvPlayer` per native session:

```kotlin
val player = MpvPlayer.create(context, listener, hookTimeoutMillis = 3_000) {
    setOption("vo", "gpu")            // pre-init options; create() sets none of its own
}
player.observeProperty("pause", PropertyFormat.Flag)
player.command("loadfile", url)       // the playlist entry id, or null
player.attachSurfaces(videoSurface, osdSurface, videoGeneration, osdGeneration)
player.close()
```

- Every call blocks. Run them off the main thread; `create`,
  `attachSurfaces`, `observeProperty`, `setLogLevel` and `close` refuse it.
- `MpvPlayer.Listener` receives `onPropertyChange`, `onEvent` and `onHook` on
  one library-owned thread per session, in mpv's order. Log lines arrive as
  `MpvEvent.LogMessage` in the same stream, so a line explaining a failed open
  comes before its end-file. mpv's event thread only enqueues: it never blocks
  and nothing is dropped.
- mpv holds playback at a hook (`on_preloaded`) until `continueHook(id)`. With
  `hookTimeoutMillis > 0` the library continues it itself that long after
  `onHook` was called; a throwing `onHook` is continued at once.
- `close()` waits for the native teardown. Once it starts no listener call
  starts, and every unanswered hook is continued.
- Replacing the OSD surface while playing needs the glue's surface-generation
  handoff, so the glue and libmpv must come from the same commit. A retirement
  that times out quarantines the old producer instead of releasing resources it
  may still use.

Playback helpers both apps share, also in `com.edde746.mpv`:

| Class | What it does |
| --- | --- |
| `GpuVoPolicy` | Pure policy, from mpv's properties and the device's answers: when a session leaves the video plane (`vo=mediacodec`) for `gpu` or `gpu-next` (`targetFor`), and how a file's decode is routed before its decoder opens (Dolby Vision options and route, software decode, tone mapping, the decode-stall watch, the BigOcean AV1 rebuild). |
| `MediaCodecQuery` | Hardware decoder lookup and classification: H.264 High 10, HEVC Main 10, AV1, BigOcean, the Dolby Vision decoders FFmpeg would open, a decoder's highest frame rate at a size. |
| `GlCapabilities` | `textureNorm16`, probed once per process on a throwaway EGL context. |
| `MpvPlaceholderSurface` | The Surface mpv holds while the app has none: a consumer that latches and drops every frame on its own GL thread. |
| `VideoRectPolicy` | The video Surface's size for fit, cover, zoom and stretch on the plane, and the `vo-mediacodec-video-rect` value. |

### Key and manifest entry

The AAR's key covers the libmpv-android key, every git-tracked file under
`platforms/android/lib` and `scripts/symbols.py`. Editing the glue moves the
AAR's key alone; anything that moves the tarballs' key moves it too. It is
recorded beside the tarballs, with one debug-symbols zip per ABI (see
[Debug symbols](../README.md#debug-symbols)):

```json
"mpv-android": {
  "key": "<key>", "asset": "mpv-android-<key>.aar", "checksum": "<sha256>",
  "symbols": {
    "arm64-v8a": {"asset": "mpv-android-<key>-arm64-v8a-symbols.zip", "checksum": "<sha256>"},
    "...": "one per ABI"
  }
}
```

### Consuming it

Gradle resolves it through an Ivy repository on the release URL, with the key
as the version and no authentication:

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

Put the sha256 from `artifacts.json` in the app's pin and in Gradle's
dependency verification.

To try an unpublished AAR, copy it to a directory as
`mpv-android-<name>.aar` and point the Ivy repository there
(`url = uri("file:///that/dir")`, pattern `[artifact]-[revision].[ext]`) with
`<name>` as the version.

### Building it

`.github/workflows/android.yml` builds the four tarballs when their key is
stale, then the AAR from that run's tarballs or the published ones
`artifacts.json` pins. `publish.yml` calls it on main and records the AAR with
`keys.py record-packages`; `ci.yml` calls it on pull requests and also builds
the device-test APK. The `.so` files compile against the tarballs' `include/`,
which carries mpv's and the patched FFmpeg's headers; no header is vendored.

Locally it needs JDK 17+, `python3`, `zip`, `unzip` and an Android SDK
(`ANDROID_HOME`); Gradle installs the NDK `toolchain/android.txt` pins.

```bash
bash platforms/android/lib/build-aar.sh                              # the tarballs artifacts.json pins
bash platforms/android/lib/build-aar.sh --key <libmpv-android key>   # any published tarball key
bash platforms/android/lib/build-aar.sh --tarballs dist/release      # tarballs built here
```

It writes `dist/release/mpv-android.aar` and one
`dist/release/mpv-android-<abi>-symbols.zip` per ABI. Every `.so` it ships
needs a GNU build-id and a debug file, so tarballs published before the
libraries were linked with `--build-id=sha1` (`f16b821414ff` among them)
cannot be packed.

### Tests

```bash
bash scripts/test_android_glue.sh   # the glue's host C++ tests (src/test/cpp) against fakes
cd platforms/android/lib
./gradlew testDebugUnitTest         # JVM tests: listener delivery, hooks, the helpers
# device test: creates a player, plays mediaUrl, attaches and replaces surfaces
./gradlew -Pmpv.tarballs=<dir> assembleDebugAndroidTest
adb install -r -t build/outputs/apk/androidTest/debug/mpv-android-debug-androidTest.apk
adb shell am instrument -w -e mediaUrl <url> com.edde746.mpv.test/androidx.test.runner.AndroidJUnitRunner
```

AGP 9 runs a library's unit tests against its debug build type only, hence
`testDebugUnitTest`.
