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

`group.json` lists components `mpv`, `ffmpeg`, `libass`, `mpv-winbuild-cmake`
and `mingw-w64`. The winbuild graph builds ~60 more dependencies (freetype,
harfbuzz, x264, dav1d, ...). Those are deliberately NOT individual
versions.json components: they track whatever `GIT_RESET`/URL pins the
winbuild commit carries, so they are keyed -- coarsely but honestly --
through the `mpv-winbuild-cmake` commit pin. Bumping that pin is the only
way their sources move, and it moves the libmpv-windows key. What the key
does not see is network drift for the few winbuild packages that themselves
track a branch tip; treat a winbuild pin bump as the refresh point for
those.

`mingw-w64` is the exception: winbuild clones its master tip at
toolchain-bootstrap time, and it defines the target ABI (headers + CRT), so
`pin_packages.py` pins it like the payload packages and `build.sh` folds its
commit into the toolchain bootstrap marker (a bump re-drives the toolchain
targets). The pin exists because the 2026-08-29 secure-API header
restructure on mingw-w64 master broke libvpl's MinGW compat macros mid-day;
llvm's `release/22.x` branch is the remaining live-fetch in the toolchain
and freezes only inside a warm cache -- pin it the same way if it ever
bites.

## CI build-state caching

A cold build bootstraps the llvm/rust cross-toolchain (hours) and ~60
dependencies, so `publish.yml` caches the whole build state between runs,
mirroring upstream winbuild's own CI: the per-arch tree
(`build/windows/<arch>`: installed prefix, ExternalProject stamps and
`.ninja_log`), the shared sources (`build/windows/src`) and the shared rust
toolchain (`build/windows/rustup`). The per-arch tree is the unit that makes
a warm run skip: ninja re-runs a step only when its stamp is stale, its
command line changed, or the edge is missing from `.ninja_log` -- caching
stamps without the log (or vice versa) rebuilds everything.

Why reusing a restored tree is sound:

- dependency sources can only move through the `mpv-winbuild-cmake` commit;
  the pin-free last restore tier reuses a tree across winbuild bumps, and
  recipe changes still land because changed step command lines dirty their
  packages;
- the packages `pin_packages.py` rewrites (mpv, ffmpeg, libass) get new step
  command lines whenever their pins or patch series change, which dirties
  exactly those packages and their dependents, and the injected
  PATCH_COMMAND resets to the pin before applying so a re-run converges
  instead of double-applying;
- `pin_packages.py` suppresses upstream's check-git step. Upstream injects
  it at configure time whenever a source dir already exists, so the step is
  absent from a cold build's graph; its first warm appearance has no
  `.ninja_log` entry and cascades a full rebuild through the stamp chain,
  and its gitclone-lastrun.txt overwrite would let a source restored from a
  different pin state build as if it were the current pin. Suppression
  keeps the graph identical between cold and warm runs -- which is what
  makes a warm run a true no-op -- while a pin bump still invalidates
  through the gitinfo.txt content change and the vanilla clone-script
  staleness compare;
- packages that track a branch tip upstream are frozen by their stamps on a
  warm tree -- tighter, not looser, than a cold rebuild that would fetch the
  tip of the day;
- the toolchain is never re-driven on a warm tree: `build.sh` skips
  `ninja llvm/rustup/llvm-clang` while its bootstrap marker matches the
  toolchain generation + winbuild pin, and re-drives them (incrementally)
  when it does not;
- `build.sh` fullcleans and rebuilds mpv on every run: upstream's packaging
  steps embed `BUILDDATE` in their command lines and `postremovebuild`
  deletes build trees after install, so a date rollover would otherwise
  re-run copy steps against a deleted tree -- and any stale windows key
  requires relinking libmpv regardless.

ccache (`-DENABLE_CCACHE=ON`, cache inside the cached install prefix) backs
all of this up: it absorbs the compile cost of whatever a pin bump or a
bootstrap retry does re-run.

Residual: the installed prefix accumulates; a pin bump that *removes* a
library leaves its old artifacts in `install/<target>` until the cache is
cold again. Linking is name-driven via pkgconf, so a lingering library is
only reachable if something still asks for it; bump
`toolchain/windows.txt`'s generation to force a clean state when that
matters.

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
