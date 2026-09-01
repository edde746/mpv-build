# Windows platform group

Builds the libmpv dev package for Windows (x86_64, aarch64) by driving
[shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake),
pinned as the versions.json component `mpv-winbuild-cmake`.

    platforms/windows/build.sh <x86_64|aarch64> [--configure-only]
    platforms/windows/package.sh <x86_64|aarch64>   # -> libmpv-windows-<key>-<arch>.zip

`build.sh` shallow-fetches the pinned winbuild commit, runs `pin_packages.py`
to rewrite `packages/{mpv,ffmpeg,libass}.cmake` onto the versions.json pins
(upstream tracks master for all three; the rewrite uses upstream's own
`GIT_REMOTE_NAME origin` / `GIT_TAG` / `GIT_RESET` idiom from
`packages/mbedtls.cmake`, and points libass at our edde746/libass fork), then
configures with `-DCOMPILER_TOOLCHAIN=clang` (mandatory for aarch64),
bootstraps the toolchain (`ninja llvm`, `ninja rustup`, `ninja llvm-clang`)
and runs `ninja mpv`. Full builds need a Linux host with the winbuild README
apt set plus pip meson >= 1.3.0; only `--configure-only` runs elsewhere.

The zip preserves the sourceforge mpv-dev package shape (`libmpv-2.dll`,
`libmpv.dll.a`, `include/mpv/*.h` at the archive root), so consumers currently
fetching the sourceforge 7z only swap URL + hash.

aarch64 builds are Vulkan/d3d11 only: winbuild's `cmake/packages_check.cmake`
sets `-Dgl=disabled -Degl-angle=disabled` for that target.

## Key coarseness

`group.json` lists components `mpv`, `ffmpeg`, `libass`, `mpv-winbuild-cmake`.
The winbuild graph builds ~60 more dependencies (freetype, harfbuzz, x264,
dav1d, ...). Those are deliberately NOT individual versions.json components:
they track whatever `GIT_RESET`/URL pins the winbuild commit carries, so they
are keyed -- coarsely but honestly -- through the `mpv-winbuild-cmake` commit
pin. Bumping that pin is the only way their sources move, and it moves the
libmpv-windows key. What the key does not see is network drift for the few
winbuild packages that themselves track a branch tip; treat a winbuild pin
bump as the refresh point for those.

## Slimming candidates (later pass, deliberately not wave 2)

The dependency set is full-fat to keep the pinning change isolated. Candidates
to drop for a libmpv-only artifact, each needing the double edit in the
winbuild checkout -- remove from `packages/mpv.cmake` DEPENDS *and* flip the
matching meson flag in its CONFIGURE_COMMAND:

| dependency        | meson flag to flip                  |
| ----------------- | ----------------------------------- |
| vapoursynth       | `-Dvapoursynth=disabled`            |
| rubberband        | `-Drubberband=disabled`             |
| libsdl2           | `-Dsdl2-gamepad=disabled`           |
| mujs              | `-Djavascript=disabled`             |
| libbluray (via ffmpeg/mpv) | `-Dlibbluray=disabled`     |
| libdvdnav/libdvdread | `-Ddvdnav=disabled`              |
| uchardet          | `-Duchardet=disabled`               |
| libarchive        | `-Dlibarchive=disabled`             |
| openal-soft       | `-Dopenal=disabled`                 |
| lcms2             | `-Dlcms2=disabled`                  |
| libsixel          | `-Dsixel=disabled`                  |
| subrandr          | `-Dsubrandr=disabled`               |
| curl              | `-Dlibcurl=disabled`                |
| luajit            | `-Dlua=disabled`                    |
| pdf manual        | `-Dpdf-build=disabled`              |

(ffmpeg's own DEPENDS list has a parallel set -- libmodplug, libopenmpt,
libbs2b, games codecs etc. -- with `--enable-*` configure flags to drop.)
Slimming would be implemented as more rewriting in `pin_packages.py`, keeping
the winbuild checkout itself pristine.

## Testing

    python3 platforms/windows/test_pin_packages.py

runs `pin_packages.py` against byte-exact fixture copies of the real package
files (`testdata/`, provenance in `testdata/PROVENANCE`) and asserts the
injected block matches the mbedtls idiom, idempotency, the edde746/libass
repoint, and that a PATCH_COMMAND is only injected for a non-empty resolved
windows series.
