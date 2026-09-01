#!/usr/bin/env python3
"""Pin mpv-winbuild-cmake's mpv/ffmpeg/libass packages to versions.json.

mpv-winbuild-cmake tracks upstream master for its payload packages:
packages/{mpv,ffmpeg,libass}.cmake are ExternalProject_Add blocks with no
GIT_TAG and no PATCH_COMMAND. Upstream's own pinning idiom -- see
packages/mbedtls.cmake at the pinned winbuild commit, mirrored in
testdata/mbedtls.cmake -- uses its patched ExternalProject keywords:

    PATCH_COMMAND ${EXEC} git am --3way ${CMAKE_CURRENT_SOURCE_DIR}/<pkg>-*.patch
    UPDATE_COMMAND ""
    GIT_REMOTE_NAME origin
    GIT_TAG <branch-or-tag>
    GIT_RESET <commit> # <human-readable version>

This helper rewrites the payload package files (and toolchain/mingw-w64.cmake,
see EXTRA_COMPONENTS) in a winbuild checkout to that idiom, driven by
versions.json resolved pins (overrides.windows folded in):

  * GIT_REPOSITORY is rewritten to the pinned url -- libass builds from our
    edde746/libass fork, not upstream libass/libass, which is the windows
    parity win of this pinning pass;
  * GIT_REMOTE_NAME origin / GIT_TAG <commit> / GIT_RESET <commit> # <human>
    are injected directly after UPDATE_COMMAND "", like mbedtls -- except the
    GIT_TAG carries the resolved commit, because the tag value is what lands
    in <pkg>-gitinfo.txt, the only graph input that invalidates the download
    step on a warm cached tree (a textually stable branch or re-pointed tag
    must not silently keep the old source);
  * the resolved windows patch series (patches/<c>/series.common followed by
    patches/<c>/series.windows) is staged into packages/ as
    <c>-NNNN-<pool name>.patch and a PATCH_COMMAND is injected directly
    before UPDATE_COMMAND "". It resets the source to the pinned commit and
    then uses `git apply`, not `git am`: our pool patches are plain
    `git diff` output without mail headers, which git am rejects, and the
    reset makes the step idempotent -- ninja re-runs a patch step alone
    whenever only the series changed, and it must not double-apply onto an
    already-patched tree. ${EXEC} is winbuild's bash wrapper ending in
    `eval $*`, so the quoted compound runs in a shell, the glob expands
    lexically and the NNNN prefix preserves series order. An empty series
    stages nothing and injects no PATCH_COMMAND.

The rewrite is idempotent: previously injected keyword lines and previously
staged <c>-*.patch files are dropped before injecting fresh ones. That is
safe because the pristine package files carry none of the injected keywords
and the pinned winbuild commit ships no packages/<c>-*.patch for these
components (both facts are asserted by test_pin_packages.py against fixture
copies in testdata/).

The script also suppresses the check-git step in cmake/custom_steps.cmake.
Upstream injects it at configure time whenever a package's source dir already
exists, to mark an adopted source's download step as done -- but the step is
absent from a cold build's graph, so its first warm appearance cascades a
full rebuild through the stamp chain, and its lastrun overwrite would let a
source restored from a different pin state build as if it were the current
pin. With the injection suppressed the graph is identical between cold and
warm runs (what makes a warm run a true no-op) and the vanilla clone-script
staleness compare handles adoption and pin bumps honestly (see the comment
at CHECK_GIT_ORIGINAL).
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

COMPONENTS = ("mpv", "ffmpeg", "libass")
# Additional pinned packages, component -> cmake file inside the winbuild
# checkout. These pin through the same strip-before-inject rewrite but carry
# no patch series (the staged-patch glob is anchored to packages/ payload
# names). Why each is pinned:
#   mingw-w64: cloned at toolchain-bootstrap time, defines the target ABI;
#     the 2026-08-29 secure-API restructure broke libvpl mid-day.
#   llvm: the toolchain's other live-fetch (a moving release branch; its
#     pristine file carries upstream's own GIT_REMOTE_NAME/GIT_TAG, which
#     the rewrite replaces).
#   svt-av1: follows ffmpeg master; SVT-AV1 4.0 removed a field the pinned
#     release ffmpeg still sets unguarded, so the master tip cannot build
#     with a release ffmpeg.
#   nv-codec-headers: same shape; the 13.1 in-dev tip reshapes
#     NV_ENC_CLOCK_TIMESTAMP_SET, which n8.0.1's nvenc wrapper still uses.
EXTRA_COMPONENTS = {
    "mingw-w64": "toolchain/mingw-w64.cmake",
    "llvm": "toolchain/llvm/llvm.cmake",
    "svt-av1": "packages/svtav1.cmake",
    "nv-codec-headers": "packages/nvcodec-headers.cmake",
}
GROUP = "windows"

# Keywords this script owns inside the three package files. Stripped before
# every injection so re-runs converge.
INJECTED_KEYWORDS = ("GIT_REMOTE_NAME", "GIT_TAG", "GIT_RESET", "PATCH_COMMAND")


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def resolved_pins(versions, component):
    """The component's git pins with overrides.windows folded in.

    Mirrors scripts/keys.py resolved_pins(); duplicated here (it is four
    lines of folding) so this helper does not depend on the caller's cwd.
    """
    entry = versions.get("components", {}).get(component)
    if entry is None:
        fail(f"versions.json: no component {component!r}")
    pins = {f: entry[f] for f in ("version", "url", "ref", "commit") if f in entry}
    for field, value in (entry.get("overrides", {}).get(GROUP) or {}).items():
        if field in ("version", "url", "ref", "commit"):
            pins[field] = value
    for field in ("version", "url", "ref", "commit"):
        if field not in pins:
            fail(
                f"versions.json: {component} has no {field!r} for the {GROUP} group; "
                "GIT_RESET pinning needs a fully resolved git pin"
            )
    return pins


def read_series(path):
    """Series entries in file order; missing file = empty (patches.py rules)."""
    if not path.is_file():
        return []
    entries = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    return entries


def resolve_series(repo, component):
    root = repo / "patches" / component
    return read_series(root / "series.common") + read_series(root / f"series.{GROUP}")


def stage_patches(repo, component, packages_dir):
    """Copy the resolved series into packages/ under glob-ordered names."""
    for stale in sorted(packages_dir.glob(f"{component}-*.patch")):
        stale.unlink()
    staged = []
    for index, name in enumerate(resolve_series(repo, component), start=1):
        pool_file = repo / "patches" / component / "pool" / name
        if not pool_file.is_file():
            fail(f"{component}: series entry {name!r} has no pool file {pool_file}")
        target = packages_dir / f"{component}-{index:04d}-{name}"
        shutil.copyfile(pool_file, target)
        staged.append(target.name)
    return staged


def indent_of(line):
    return line[: len(line) - len(line.lstrip())]


def rewrite(text, component, pins, have_patches):
    """The pinned package file text; pure so the tests can hammer it."""
    lines = []
    for line in text.splitlines():
        word = line.strip().split(" ", 1)[0] if line.strip() else ""
        if word in INJECTED_KEYWORDS:
            continue  # previously injected by us; the pristine files have none
        lines.append(line)

    repo_lines = [i for i, l in enumerate(lines) if l.strip().startswith("GIT_REPOSITORY ")]
    if len(repo_lines) != 1:
        fail(f"{component}.cmake: expected exactly one GIT_REPOSITORY line, found {len(repo_lines)}")
    url = pins["url"]
    if not url.endswith(".git"):
        url += ".git"
    lines[repo_lines[0]] = f"{indent_of(lines[repo_lines[0]])}GIT_REPOSITORY {url}"

    update_lines = [i for i, l in enumerate(lines) if l.strip() == 'UPDATE_COMMAND ""']
    if len(update_lines) != 1:
        fail(f'{component}.cmake: expected exactly one UPDATE_COMMAND "" line, found {len(update_lines)}')
    at = update_lines[0]
    pad = indent_of(lines[at])

    # GIT_TAG carries the resolved commit, not the human ref. The tag value
    # is what ExternalProject records in <pkg>-gitinfo.txt, and that file's
    # content is the ONLY graph input that invalidates the download step on
    # a warm cached tree: a ref that stays textually identical while its
    # target moves (a branch like mingw-w64's master, or a re-pointed tag)
    # would otherwise leave the old source in place silently. GIT_RESET
    # names the same commit for winbuild's reset/force-update machinery and
    # keeps the human-readable ref and version in its comment.
    comment = pins["version"] if pins["ref"] in pins["version"] else f"{pins['ref']} {pins['version']}"
    after = [
        f"{pad}GIT_REMOTE_NAME origin",
        f"{pad}GIT_TAG {pins['commit']}",
        f"{pad}GIT_RESET {pins['commit']} # {comment}",
    ]
    before = []
    if have_patches:
        # One quoted argument: ${EXEC} is `eval $*`, so the compound runs in a
        # shell. The reset makes a re-run of the patch step alone (series-only
        # change, or a step cascade) converge instead of double-applying.
        before = [
            f'{pad}PATCH_COMMAND ${{EXEC}} "git reset --hard {pins["commit"]} -q '
            f'&& git apply ${{CMAKE_CURRENT_SOURCE_DIR}}/{component}-*.patch"'
        ]
    lines[at:at + 1] = before + [lines[at]] + after
    return "\n".join(lines) + "\n"


# The check-git step upstream injects at configure time when a package source
# dir already exists (cmake/custom_steps.cmake force_rebuild_git). It exists
# to mark an adopted source's download step as done: it touches the download
# stamp and overwrites gitclone-lastrun.txt with the current gitinfo.txt.
# Both halves are wrong for this pipeline:
#
#   * the step is absent from the cold build's graph (no source dir at
#     configure time), so its first warm appearance has no .ninja_log entry;
#     every step that runs touches its stamp, so the whole package chain
#     cascades through configure/build/install once per adoption -- and its
#     touch of the download stamp re-triggers that cascade on every run;
#   * the lastrun overwrite claims whatever state the adopted source is in
#     IS the current pin; a source tree restored from a different pin state
#     would silently build the wrong source under a fresh content key.
#
# Suppressing the injection keeps the ExternalProject graph identical between
# cold and warm runs, which is what makes a warm run a true no-op. The
# vanilla staleness machinery stays honest on its own: a pin bump rewrites
# gitinfo.txt, dirtying the download step, and a missing/stale
# gitclone-lastrun.txt makes the clone script re-clone for real.
CHECK_GIT_ORIGINAL = "    if(EXISTS ${source_dir}/.git)\n"
CHECK_GIT_NEUTRALIZED = (
    "    if(FALSE) # check-git suppressed for warm-cache correctness "
    "(pin_packages.py)\n"
)


def neutralize_check_git(text):
    """custom_steps.cmake text with the cache-hostile check-git commands
    replaced; pure so the tests can hammer it."""
    if CHECK_GIT_NEUTRALIZED in text:
        return text
    if CHECK_GIT_ORIGINAL not in text:
        fail(
            "cmake/custom_steps.cmake: the check-git step no longer matches the "
            "audited idiom; re-audit the cache-cascade rewrite against the new "
            "winbuild commit before pinning to it"
        )
    return text.replace(CHECK_GIT_ORIGINAL, CHECK_GIT_NEUTRALIZED, 1)


# winbuild enables cuda for every arch (2026-08-04, "ffmpeg: enable cuda for
# aarch64") against ffmpeg master. Under the pinned release ffmpeg the
# aarch64-w64-mingw32 ffnvcodec probe fails and configure aborts, so the
# flags are rewritten behind an arch guard: x86_64 keeps cuda, aarch64 drops
# it. Upstream builds this combination against master only; nobody tests
# release-ffmpeg aarch64 cuda.
FFMPEG_CUDA_ORIGINAL = (
    "        --enable-cuda-llvm\n"
    "        --enable-cuvid\n"
    "        --enable-nvdec\n"
    "        --enable-nvenc\n"
)
FFMPEG_CUDA_GATED = "        ${ffmpeg_cuda}\n"
FFMPEG_CUDA_GUARD = (
    "# cuda gated by pin_packages.py: the pinned release ffmpeg's ffnvcodec\n"
    "# probe fails for aarch64-w64-mingw32 (upstream enables cuda for aarch64\n"
    "# against ffmpeg master only).\n"
    'if(NOT TARGET_CPU STREQUAL "aarch64")\n'
    "    set(ffmpeg_cuda --enable-cuda-llvm --enable-cuvid --enable-nvdec --enable-nvenc)\n"
    "endif()\n"
)


# winbuild's mpv recipe targets mpv master; the pinned release lacks two of
# its meson options (subrandr and libcurl landed after v0.41.0), and meson
# hard-errors on unknown options. The subrandr DEPENDS entry stays: the
# library just goes unused. Re-audit when the mpv pin moves past them.
MPV_MASTER_ONLY_OPTIONS = (
    "        -Dsubrandr=enabled\n",
    "        -Dlibcurl=enabled\n",
)


def gate_mpv_options(text):
    """mpv.cmake text without the master-only meson options; pure for tests."""
    if not any(option in text for option in MPV_MASTER_ONLY_OPTIONS):
        if "-Dsubrandr" in text or "-Dlibcurl" in text:
            fail(
                "packages/mpv.cmake: master-only meson options no longer match "
                "the audited shape; re-audit the option gate against the new "
                "winbuild commit before pinning to it"
            )
        return text
    for option in MPV_MASTER_ONLY_OPTIONS:
        text = text.replace(option, "")
    return text


def gate_ffmpeg_cuda(text):
    """ffmpeg.cmake text with the cuda enables arch-gated; pure for tests."""
    if FFMPEG_CUDA_GATED in text and text.startswith(FFMPEG_CUDA_GUARD):
        return text
    if FFMPEG_CUDA_ORIGINAL not in text:
        fail(
            "packages/ffmpeg.cmake: the cuda enable flags no longer match the "
            "audited shape; re-audit the aarch64 cuda gate against the new "
            "winbuild commit before pinning to it"
        )
    return FFMPEG_CUDA_GUARD + text.replace(FFMPEG_CUDA_ORIGINAL, FFMPEG_CUDA_GATED, 1)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--winbuild", required=True, help="mpv-winbuild-cmake checkout to rewrite")
    parser.add_argument(
        "--repo",
        default=str(Path(__file__).resolve().parent.parent.parent),
        help="unified repo root holding versions.json and patches/ (default: this file's repo)",
    )
    args = parser.parse_args(argv)

    repo = Path(args.repo)
    winbuild = Path(args.winbuild)
    packages_dir = winbuild / "packages"
    if not packages_dir.is_dir():
        fail(f"{winbuild}: not a mpv-winbuild-cmake checkout (no packages/ directory)")

    versions_path = repo / "versions.json"
    if not versions_path.is_file():
        fail(f"{versions_path}: missing")
    versions = json.loads(versions_path.read_text(encoding="utf-8"))

    for component in COMPONENTS:
        pins = resolved_pins(versions, component)
        staged = stage_patches(repo, component, packages_dir)
        package_file = packages_dir / f"{component}.cmake"
        if not package_file.is_file():
            fail(f"{package_file}: missing")
        text = package_file.read_text(encoding="utf-8")
        pinned = rewrite(text, component, pins, bool(staged))
        if component == "ffmpeg":
            pinned = gate_ffmpeg_cuda(pinned)
        if component == "mpv":
            pinned = gate_mpv_options(pinned)
        if pinned != text:
            package_file.write_text(pinned, encoding="utf-8")
        patched = f", {len(staged)} patch(es) staged" if staged else ""
        print(f"pinned {component} -> {pins['ref']} @ {pins['commit'][:10]}{patched}")

    for component, relpath in EXTRA_COMPONENTS.items():
        pins = resolved_pins(versions, component)
        toolchain_file = winbuild / relpath
        if not toolchain_file.is_file():
            fail(f"{toolchain_file}: missing")
        text = toolchain_file.read_text(encoding="utf-8")
        pinned = rewrite(text, component, pins, have_patches=False)
        if pinned != text:
            toolchain_file.write_text(pinned, encoding="utf-8")
        print(f"pinned {component} -> {pins['ref']} @ {pins['commit'][:10]}")

    custom_steps = winbuild / "cmake" / "custom_steps.cmake"
    if not custom_steps.is_file():
        fail(f"{custom_steps}: missing")
    steps_text = custom_steps.read_text(encoding="utf-8")
    neutralized = neutralize_check_git(steps_text)
    if neutralized != steps_text:
        custom_steps.write_text(neutralized, encoding="utf-8")
    print("neutralized the check-git cache cascade in cmake/custom_steps.cmake")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
