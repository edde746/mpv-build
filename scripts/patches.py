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

    patches.py resolve <component> <platform> [--hashes]
        Print the resolved series (series.common then series.<platform>) as
        pool-relative filenames, one per line. With --hashes each line is
        `name<TAB>sha256hex` of the pool file's bytes.

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
"""

import hashlib
import subprocess
import sys
from pathlib import Path

PLATFORMS = ("apple", "android", "linux", "windows")
PATCHES_DIR = Path("patches")


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


def cmd_resolve(component, platform, hashes):
    require_platform(platform)
    pool = PATCHES_DIR / component / "pool"
    for name in resolve(component, platform):
        if hashes:
            path = pool / name
            if not path.is_file():
                fail(f"{component}: series entry {name!r} has no pool file {path}")
            print(f"{name}\t{sha256_file(path)}")
        else:
            print(name)
    return 0


def cmd_check():
    problems = []

    if not PATCHES_DIR.is_dir():
        # A tree with no patches at all is valid.
        return 0

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
                if "/" in name or name != name.strip():
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
    if len(argv) >= 3 and argv[0] == "resolve":
        hashes = "--hashes" in argv[3:]
        extra = [a for a in argv[3:] if a != "--hashes"]
        if len(argv) < 3 or extra:
            fail("usage: patches.py resolve <component> <platform> [--hashes]")
        return cmd_resolve(argv[1], argv[2], hashes)
    if argv == ["check"]:
        return cmd_check()
    if len(argv) >= 4 and argv[0] == "apply":
        check_only = "--check" in argv[4:]
        extra = [a for a in argv[4:] if a != "--check"]
        if extra:
            fail("usage: patches.py apply <component> <platform> <srcdir> [--check]")
        return cmd_apply(argv[1], argv[2], argv[3], check_only)
    fail(
        "usage: patches.py resolve <component> <platform> [--hashes] | "
        "patches.py check | patches.py apply <component> <platform> <srcdir> [--check]"
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
