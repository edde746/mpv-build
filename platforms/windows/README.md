# Windows platform group

Builds the libmpv dev package for Windows (x86_64, aarch64) by driving
[shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake),
pinned as the versions.json component `mpv-winbuild-cmake`.

    platforms/windows/build.sh <x86_64|aarch64> [--configure-only]
    platforms/windows/package.sh <x86_64|aarch64>   # -> libmpv-windows-<key>-<arch>.zip
                                                    #  + libmpv-windows-<arch>-symbols.zip

`build.sh` shallow-fetches the pinned winbuild commit, runs `pin_packages.py`
to rewrite every source winbuild builds onto the versions.json pins -- the
payload packages (`packages/{mpv,ffmpeg,libass}.cmake`, whose upstream tracks
master) plus winbuild's live-fetch toolchain and ffmpeg-side packages
(`toolchain/mingw-w64.cmake`, `toolchain/llvm/llvm.cmake`,
`packages/svtav1.cmake`, `packages/nvcodec-headers.cmake`) and OpenSSL
(`packages/openssl.cmake`, which winbuild pins to an unreleased master
snapshot) -- using upstream's
own `GIT_REMOTE_NAME origin` / `GIT_TAG` / `GIT_RESET` idiom from
`packages/mbedtls.cmake`, then
configures with `-DCOMPILER_TOOLCHAIN=clang` (mandatory for aarch64),
bootstraps the toolchain (`ninja llvm`, `ninja rustup`, `ninja llvm-clang`)
and runs `ninja mpv`. Full builds need a Linux host with the winbuild README
apt set plus pip meson >= 1.3.0; only `--configure-only` runs elsewhere.

The zip preserves the sourceforge mpv-dev package shape (`libmpv-2.dll`,
`libmpv.dll.a`, `include/mpv/*.h` at the archive root), so consumers currently
fetching the sourceforge 7z only swap URL + hash. Beside them sits
`licenses/` (see "Licence texts").

aarch64 builds are Vulkan/d3d11 only: winbuild's `cmake/packages_check.cmake`
sets `-Dgl=disabled -Degl-angle=disabled` for that target.

## Licence texts

`licenses/<component>/` holds the licence texts of every component linked
into `libmpv-2.dll`, copied unchanged from the source tree the build compiled,
at their path in that tree. `package.sh` collects them with
`scripts/licenses.py` before zipping, and fails when a component yields no
text or a keyed component has no folder.

`pin_packages.py --print-licence-sources build/windows/<arch>` derives the set
from the configured build tree. `ninja -t inputs mpv` names every package whose
steps the mpv target waits for (85 on aarch64, 87 on x86_64 at the pinned
winbuild commit). Of those, the packages that put no code into the DLL are
dropped: curl and the libraries only it links, subrandr, and the Vulkan loader,
which is a DLL of its own. x265's bit-depth builds fold into x265, and
opus-dnn's models into opus. The toolchain runtime clang links in statically
is added: llvm (compiler-rt's builtins, libunwind, libc++, libc++abi) and
mingw-w64 (the CRT and winpthreads). Each package's source dir is the one
its ExternalProject recorded beside its stamps. Folders take the versions.json
name for a pinned package (`svt-av1`, `nv-codec-headers`) and the winbuild
package name otherwise (`freetype2`, `libjpeg`). The whole graph counts, so a
header package an arch never includes (angle-headers on aarch64) still gets a
folder. `mpv-winbuild-cmake` gets none: it publishes no licence, and its code
in the DLL is patches to packages whose texts ship.

`GROUP_RULES["windows"]` in `scripts/licenses.py` adds what is not a root
licence file. That covers vendored code the build compiles in (FAST in aom and
SVT-AV1, x86inc, openal-soft's {fmt} and GSL, SDL's HIDAPI and YUV converters,
libopenmpt's mpt, x265's HDR10+ library) and licences that exist only as source
comments (avisynth-headers, nv-codec-headers, PFFFT and Ooura's FFT in libsoxr,
the Speex resampler in libmysofa). It also adds the llvm and mingw-w64 runtime
subtrees. Sparse checkouts that leave the root licence out (angle-headers,
amf-headers, vapoursynth) have it read from git. The not-linked list and the
rules describe the pinned graph; re-audit both when the winbuild or mpv pin
moves.

## Key coarseness

`group.json` lists components `mpv`, `ffmpeg`, `libass`,
`mpv-winbuild-cmake`, `mingw-w64`, `llvm`, `svt-av1`, `nv-codec-headers`
and `openssl` (the last five being the packages
`pin_packages.EXTRA_COMPONENTS` pins). The winbuild graph builds ~60 more dependencies (freetype,
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
`pin_packages.py` pins it like the payload packages; a bump re-drives the
dirty toolchain subgraph through the always-run toolchain targets. The pin
exists because the 2026-08-29 secure-API header restructure on mingw-w64
master broke libvpl's MinGW compat macros mid-day; llvm's `release/22.x`
branch is the remaining live-fetch in the toolchain and freezes only inside
a warm cache -- pin it the same way if it ever bites.

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
- the packages `pin_packages.py` pins get new step command lines whenever
  their pins or patch series change, which dirties exactly those packages and
  their dependents, and the injected PATCH_COMMAND resets to the pin before
  applying so a re-run converges instead of double-applying; `build.sh`
  invalidates stale sources from the same pin table, so a package added to
  the pinning side cannot slip past invalidation;
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
- the toolchain targets (`ninja llvm/rustup/llvm-clang`) run on every
  build; on a clean warm tree they are stamp-driven no-ops (~50ms), and a
  moved pin re-drives exactly its dirty subgraph. There is deliberately no
  skip-if-bootstrapped marker: one existed and recorded intent instead of
  reality, serving stale mingw-w64 headers after a pin move;
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

## Reproducible bytes

A rebuild of a published key has to reproduce the published zip byte for
byte (`keys.py publish-assets` refuses anything else), so nothing in it may
record when it was built. Key 442b5d101e4c's `libmpv-2.dll` recorded five
clocks: the PE header and debug directory `TimeDateStamp` lld-link takes at
link time, so every rebuild differed; and OpenSSL's "built on" line, SVT-AV1's
and mpv's `__DATE__ __TIME__` and davs2's build time, which the cached tree
had kept from the runs that compiled them.

`build.sh` exports `SOURCE_DATE_EPOCH=1577836800` (2020-01-01, the zip's
mtime), which lld-link uses as the `TimeDateStamp`, clang as `__DATE__` and
`__TIME__`, and OpenSSL as its "built on" date. davs2's `version.sh` reads
`date`, so `pin_packages.py` adds a step after davs2's configure that rewrites
the generated `XBUILD_TIME` to the same moment. A cached tree compiled before
the epoch still holds the old stamps, and winbuild's ccache, configured with
`sloppiness=time_macros`, hands back objects with the date of whichever
compile filled it. So `build.sh` rebuilds every package once, from an empty
compile cache, in a tree without `.source-date-epoch` (the toolchain is kept),
and writes the marker before building.

The zip itself is sorted, `-X`, with every mtime at 2020-01-01 and every mode
at `u+rw,go-w,a+rX`, as the apple zips are, so the umask has no say. The build
leg runs `scripts/test_build_date.sh` on it before upload.

A rebuild only reproduces from a restored tree: 57 of the 95 git packages at
the pinned winbuild commit name no `GIT_TAG` or `GIT_RESET` and clone a branch
tip, so a cold rebuild of the same key fetches different sources; see "Key
coarseness".

### Debug symbols

Each leg also publishes `libmpv-2.pdb`, the PDB lld writes when winbuild
links mpv with `PDB=1`, so crash reports from the DLL symbolicate. Upstream's
copy-binary step saves only `mpv.pdb` (`${mpv_copy_debug}`) and
`cleanup(mpv copy-package-dir)` then deletes the build dir, so `pin_packages.py`
adds one `cmake -E copy` of `libmpv-2.pdb` into `mpv-debug/` right after that
line; copy-package-dir moves the tree to `mpv-debug-<cpu>-<date>-git-<sha>/`
beside the dev tree. The copy only reads what the link wrote, so the DLL's
bytes are those of a build without it. `package.sh` packs the PDB with
`scripts/symbols.py pack` into `dist/release/libmpv-windows-<arch>-symbols.zip`,
which fails unless the PDB's GUID and age are the ones `libmpv-2.dll`'s
CodeView record names, and `keys.py record-platform` renames it to
`libmpv-windows-<key>-<arch>-symbols.zip`. The build leg's
`test_build_date.sh dist/release/*.zip` scans it too.

It reproduces with the DLL: lld sets the PDB's GUID, and so the DLL's
CodeView record, from a hash of the whole PDB file (the GUID's last eight
bytes spell `LLD PDB.`), and `symbols.py` writes the zip sorted, `-X`, with
2020-01-01 mtimes and mode 0644. The corollary is that nothing can shrink the
PDB without changing the DLL. Expect roughly 350 MiB: zhongfly's mpv.pdb from
a build of the same recipe measures 352.7 MiB, about 88 MiB zipped. The
winbuild compiler wrapper appends `-gcodeview` after every package's own
flags, and as the last `-g` option it turns full debug info on for all of
them, so libjxl, shaderc and libavcodec dominate that PDB. lld already merges
types by global hash (`--pdb=` makes it `/DEBUG:FULL`, which implies GHASH; a
link-time speedup, not a size one), and a package's own `-gline-tables-only`
would lose to the wrapper's trailing `-gcodeview`.

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

runs the real `main()` over a synthetic winbuild checkout planted from the
byte-exact fixture copies in `testdata/` (provenance and digests in
`testdata/PROVENANCE`), against this repo's versions.json and patch series,
and asserts what it leaves behind: every pinned package carrying its resolved
commit, the staged series and its PATCH_COMMAND (injected exactly when the
resolved series is non-empty), the neutralized check-git step, and a second
run converging. The remaining tests pin the pure rewrites -- the injected
block matches the mbedtls idiom, the aarch64 cuda gate, the mpv option
gate, the libmpv-2.pdb capture, and the davs2 build-time rewrite -- and the
licence-source query: the linked set read from `ninja -t inputs mpv` as ninja
1.11 and 1.13 print it, the folder names `group.json` keys, and the source
dirs read from a synthetic build tree.
