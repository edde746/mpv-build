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

This helper rewrites the three package files in a winbuild checkout to that
idiom, driven by versions.json resolved pins (overrides.windows folded in):

  * GIT_REPOSITORY is rewritten to the pinned url -- libass builds from our
    edde746/libass fork, not upstream libass/libass, which is the windows
    parity win of this pinning pass;
  * GIT_REMOTE_NAME origin / GIT_TAG <ref> / GIT_RESET <commit> # <version>
    are injected directly after UPDATE_COMMAND "", exactly like mbedtls;
  * the resolved windows patch series (patches/<c>/series.common followed by
    patches/<c>/series.windows) is staged into packages/ as
    <c>-NNNN-<pool name>.patch and a PATCH_COMMAND is injected directly
    before UPDATE_COMMAND "". It uses `git apply`, not `git am`: our pool
    patches are plain `git diff` output without mail headers, which git am
    rejects. ${EXEC} is winbuild's bash wrapper ending in `eval $*`, so the
    glob expands lexically and the NNNN prefix preserves series order. An
    empty series stages nothing and injects no PATCH_COMMAND.

The rewrite is idempotent: previously injected keyword lines and previously
staged <c>-*.patch files are dropped before injecting fresh ones. That is
safe because the pristine package files carry none of the injected keywords
and the pinned winbuild commit ships no packages/<c>-*.patch for these
components (both facts are asserted by test_pin_packages.py against fixture
copies in testdata/).
"""

import argparse
import json
import shutil
import sys
from pathlib import Path

COMPONENTS = ("mpv", "ffmpeg", "libass")
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

    after = [
        f"{pad}GIT_REMOTE_NAME origin",
        f"{pad}GIT_TAG {pins['ref']}",
        f"{pad}GIT_RESET {pins['commit']} # {pins['version']}",
    ]
    before = []
    if have_patches:
        before = [
            f"{pad}PATCH_COMMAND ${{EXEC}} git apply "
            f"${{CMAKE_CURRENT_SOURCE_DIR}}/{component}-*.patch"
        ]
    lines[at:at + 1] = before + [lines[at]] + after
    return "\n".join(lines) + "\n"


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
        if pinned != text:
            package_file.write_text(pinned, encoding="utf-8")
        patched = f", {len(staged)} patch(es) staged" if staged else ""
        print(f"pinned {component} -> {pins['ref']} @ {pins['commit'][:10]}{patched}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
