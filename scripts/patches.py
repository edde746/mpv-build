#!/usr/bin/env python3
"""Series-based patch framework shared by every platform driver.

Layout, relative to the repository root (the current working directory):

    patches/<component>/pool/<name>.patch   the patch bytes, platform-neutral
    patches/<component>/series.common       applied on every platform, first
    patches/<component>/series.<platform>   applied after series.common

A series file lists one pool filename per line; `#` comments and blank lines
are ignored. The order of lines is authoritative -- nothing is ever sorted. A
missing series file is an empty series, and a component without a patches
directory simply has no patches. Platform groups are apple, android, linux and
windows.

Commands:

    patches.py resolve <component> <platform>
        Print the resolved series (series.common then series.<platform>) as
        pool-relative filenames, one per line.

    patches.py check
        Validate every component: each series entry names an existing pool
        file, every pool file is referenced by at least one series, and no
        resolved series contains the same entry twice. Exits 1 with
        diagnostics on stderr when anything is off.

    patches.py apply <component> <platform> <srcdir> [--check]
        `git apply` each resolved patch in order with <srcdir> as the
        working directory, failing on the first patch that does not apply.
        With --check the whole stack is still applied for real -- later
        patches may build on earlier hunks, so a per-patch `git apply
        --check` against the pristine tree would report false failures --
        and then unwound with `git apply -R` in reverse order, leaving the
        tree byte-identical to how it was found.

    patches.py fetch-pinned <component> <platform> <srcdir>
        Clone the exact source `<component>` is pinned to for `<platform>`
        in versions.json (override-aware), prove HEAD is the pinned commit,
        and apply the resolved series. This is the one implementation of
        the pin contract; the host regression harnesses call it instead of
        each carrying their own copy.

`check` also enforces versions.json's `platforms` arrays: a component a
group's build consumes must declare that group.
"""

import hashlib
import json
import subprocess
import sys
from pathlib import Path

PLATFORMS = ("apple", "android", "linux", "windows")
PATCHES_DIR = Path("patches")
VERSIONS_PATH = Path("versions.json")


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_series(path):
    """Entries of one series file, in file order. Missing file = empty."""
    if not path.is_file():
        return []
    entries = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entries.append(line)
    return entries


def series_files(component, platform):
    """The series files that make up one resolved series, in order."""
    root = PATCHES_DIR / component
    return [root / "series.common", root / f"series.{platform}"]


def resolve(component, platform):
    """Resolved series for one component and platform, in authoritative order."""
    entries = []
    for path in series_files(component, platform):
        entries.extend(read_series(path))
    return entries


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_platform(platform):
    if platform not in PLATFORMS:
        fail(f"unknown platform {platform!r}; expected one of {', '.join(PLATFORMS)}")


def cmd_resolve(component, platform):
    require_platform(platform)
    for name in resolve(component, platform):
        print(name)
    return 0


def component_pins(component, platform):
    """The component's pins for one platform, with overrides.<platform> folded."""
    if not VERSIONS_PATH.is_file():
        fail(f"{VERSIONS_PATH}: missing; component pins live there now")
    components = json.loads(VERSIONS_PATH.read_text(encoding="utf-8")).get("components", {})
    entry = components.get(component)
    if entry is None:
        fail(f"{VERSIONS_PATH}: no component {component!r}")
    pins = dict(entry)
    pins.update((entry.get("overrides") or {}).get(platform) or {})
    return pins


def cmd_fetch(component, platform, srcdir):
    """Clone the component's pinned source for this platform and patch it.

    The single implementation of the pin contract: resolve the override-aware
    pin, shallow-clone the ref, prove HEAD is the pinned commit, then apply the
    resolved series.
    """
    require_platform(platform)
    pins = component_pins(component, platform)
    if pins.get("kind") != "git":
        fail(f"{component}: {platform} pins kind {pins.get('kind')!r}, not a git checkout")
    url, ref, commit = pins.get("url"), pins.get("ref"), pins.get("commit")
    if not url or not ref:
        fail(f"{VERSIONS_PATH}: {component} has no {platform} url/ref to clone")
    if not commit:
        fail(f"{VERSIONS_PATH}: {component} has no {platform} commit to verify against")

    destination = Path(srcdir)
    if destination.exists():
        fail(f"{srcdir!r} already exists; refusing to clone into it")
    result = subprocess.run(
        ["git", "clone", "--quiet", "--depth", "1", "--branch", ref, url, str(destination)]
    )
    if result.returncode != 0:
        fail(f"{component}: could not clone {url} at {ref}")
    head = subprocess.run(
        ["git", "-C", str(destination), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if head != commit:
        fail(f"{component}: {ref} is {head}, {VERSIONS_PATH} pins {commit}")
    return cmd_apply(component, platform, srcdir, False)


def group_components():
    """group -> the versions.json components that group's build consumes."""
    consumed = {}
    for path in sorted(Path("platforms").glob("*/group.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        names = set()
        for spec in (data.get("artifacts") or {}).values():
            names.update(spec.get("components") or ())
        consumed[path.parent.name] = names
    # The apple group is defined in the Swift driver rather than a group.json:
    # the components it builds are the ones its own patch series touch.
    apple = consumed.setdefault("apple", set())
    if PATCHES_DIR.is_dir():
        for component_dir in sorted(PATCHES_DIR.iterdir()):
            if component_dir.is_dir() and read_series(component_dir / "series.apple"):
                apple.add(component_dir.name)
    return consumed


def check_platform_declarations(components, problems):
    """Every component a group builds must declare that group in versions.json.

    The `platforms` arrays are the only place that records which platforms a
    component participates in; without this they are unread decoration that can
    silently disagree with the drivers.
    """
    for group, names in sorted(group_components().items()):
        for component in sorted(names):
            entry = components.get(component)
            if entry is None:
                problems.append(
                    f"{group} builds {component!r}, which {VERSIONS_PATH} does not pin"
                )
                continue
            declared = entry.get("platforms") or []
            if group not in declared:
                problems.append(
                    f"{group} builds {component!r}, but its {VERSIONS_PATH} "
                    f"platforms {declared} do not list {group}"
                )


def cmd_check():
    problems = []
    if VERSIONS_PATH.is_file():
        components = json.loads(VERSIONS_PATH.read_text(encoding="utf-8")).get("components", {})
        check_platform_declarations(components, problems)

    if not PATCHES_DIR.is_dir():
        # A tree with no patches at all is valid.
        if not problems:
            return 0
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1

    for component_dir in sorted(PATCHES_DIR.iterdir()):
        if not component_dir.is_dir():
            problems.append(f"{component_dir}: stray file; expected component directories only")
            continue
        component = component_dir.name
        pool_dir = component_dir / "pool"

        pool_files = set()
        if pool_dir.is_dir():
            for entry in sorted(pool_dir.iterdir()):
                if entry.name.startswith("."):
                    continue
                if not entry.is_file():
                    problems.append(f"{component}: {entry} is not a regular file")
                    continue
                pool_files.add(entry.name)

        known_series = {"series.common"} | {f"series.{p}" for p in PLATFORMS}
        for entry in sorted(component_dir.iterdir()):
            if entry.name.startswith(".") or entry == pool_dir:
                continue
            if entry.name not in known_series:
                problems.append(
                    f"{component}: unexpected file {entry.name!r}; expected pool/ and "
                    f"series.common or series.<{('|'.join(PLATFORMS))}>"
                )

        referenced = set()
        for series in known_series:
            for name in read_series(component_dir / series):
                if "/" in name:
                    problems.append(f"{component}/{series}: invalid entry {name!r}")
                    continue
                referenced.add(name)
                if name not in pool_files:
                    problems.append(f"{component}/{series}: entry {name!r} has no pool file")

        for name in sorted(pool_files - referenced):
            problems.append(f"{component}: pool file {name!r} is not referenced by any series")

        for platform in PLATFORMS:
            seen = set()
            for name in resolve(component, platform):
                if name in seen:
                    problems.append(
                        f"{component}: {name!r} appears twice in the resolved {platform} series"
                    )
                seen.add(name)

    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1
    return 0


def cmd_apply(component, platform, srcdir, check_only):
    require_platform(platform)
    src = Path(srcdir)
    if not src.is_dir():
        fail(f"source directory {srcdir!r} does not exist")
    pool = (PATCHES_DIR / component / "pool").resolve()

    def unwind(applied):
        for patch in reversed(applied):
            result = subprocess.run(["git", "apply", "-R", str(patch)], cwd=src)
            if result.returncode != 0:
                fail(
                    f"{component}: could not unwind {patch.name}; "
                    f"{srcdir!r} is left with a partially applied series"
                )

    applied = []
    for name in resolve(component, platform):
        patch = pool / name
        if not patch.is_file():
            if check_only:
                unwind(applied)
            fail(f"{component}: series entry {name!r} has no pool file {patch}")
        result = subprocess.run(["git", "apply", str(patch)], cwd=src)
        if result.returncode != 0:
            if check_only:
                unwind(applied)
            fail(f"{component}: git apply failed on {name}")
        applied.append(patch)
        print(f"{'checked' if check_only else 'applied'} {name}")
    if check_only:
        unwind(applied)
    return 0


def main(argv):
    if len(argv) == 3 and argv[0] == "resolve":
        return cmd_resolve(argv[1], argv[2])
    if argv == ["check"]:
        return cmd_check()
    if len(argv) == 4 and argv[0] == "fetch-pinned":
        return cmd_fetch(argv[1], argv[2], argv[3])
    if len(argv) >= 4 and argv[0] == "apply":
        check_only = "--check" in argv[4:]
        extra = [a for a in argv[4:] if a != "--check"]
        if extra:
            fail("usage: patches.py apply <component> <platform> <srcdir> [--check]")
        return cmd_apply(argv[1], argv[2], argv[3], check_only)
    fail(
        "usage: patches.py resolve <component> <platform> | "
        "patches.py check | patches.py apply <component> <platform> <srcdir> [--check] | "
        "patches.py fetch-pinned <component> <platform> <srcdir>"
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
