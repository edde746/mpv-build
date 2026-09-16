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
        Acquire the exact source `<component>` is pinned to for `<platform>`
        in versions.json (override-aware) and apply the resolved series,
        whichever kind that pin is. A `git` pin is a shallow clone at the
        pinned ref whose HEAD is then proven equal to the pinned commit; an
        `archive` pin is a download whose sha256 must match before anything
        is extracted. This is the one implementation of the pin contract; the
        host regression harnesses call it instead of each carrying their own
        copy.

`check` also enforces versions.json's `platforms` arrays: a component a
group's build consumes must declare that group. A group's components come from
its `platforms/<group>/group.json`, or -- for apple, which has no group.json --
from the Swift driver's own `builds:` list, so every library that driver
compiles is covered and not only the ones a series file or the manifest
happens to mention.
"""

import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path

PLATFORMS = ("apple", "android", "linux", "windows")
PATCHES_DIR = Path("patches")
VERSIONS_PATH = Path("versions.json")

# The apple group has no group.json: its build is defined by this Swift driver.
APPLE_DRIVER = Path("Sources/BuildScripts/XCFrameworkBuild/main.swift")

# Swift `Library` rawValues whose canonical versions.json component is not just
# the lowercased name. The driver names libraries in its own spelling
# (`libmpv`, `FFmpeg`), versions.json keys them canonically, and this is where
# the two meet: keys.py's artifact names go through here too.
COMPONENT_ALIASES = {"libmpv": "mpv"}


def canonical_component(artifact: str) -> str:
    return COMPONENT_ALIASES.get(artifact, artifact.lower())


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
    """Materialise the component's pinned source for this platform and patch it.

    The single implementation of the pin contract: resolve the override-aware
    pin, acquire the source (a shallow clone at the pinned ref, or an archive
    whose sha256 is checked), and apply the resolved series.
    """
    require_platform(platform)
    pins = component_pins(component, platform)
    kind = pins.get("kind")
    destination = Path(srcdir)
    if destination.exists():
        fail(f"{srcdir!r} already exists; refusing to write into it")

    if kind == "git":
        _fetch_git(component, pins, destination)
    elif kind == "archive":
        _fetch_archive(component, pins, destination)
    else:
        fail(f"{component}: {platform} pins kind {kind!r}; cannot acquire that")

    return cmd_apply(component, platform, srcdir, False)


def _fetch_git(component, pins, destination):
    url, ref, commit = pins.get("url"), pins.get("ref"), pins.get("commit")
    if not url or not ref:
        fail(f"{VERSIONS_PATH}: {component} has no url/ref to clone")
    if not commit:
        fail(f"{VERSIONS_PATH}: {component} has no commit to verify against")
    # A source mirror (code.videolan.org is the only git source this repository
    # fetches and has refused connections for minutes at a time) is covered by
    # the same commit pin, so a mirror can only supply the same tree or fail.
    sources = [url] + ([pins["mirror"]] if pins.get("mirror") else [])
    clone = None
    for source in sources:
        for attempt in range(1, 4):
            result = subprocess.run(
                ["git", "clone", "--quiet", "--depth", "1", "--branch", ref, source, str(destination)]
            )
            if result.returncode == 0:
                clone = source
                break
            shutil.rmtree(destination, ignore_errors=True)
            if attempt < 3:
                time.sleep(attempt)
        if clone:
            break
        print(f"could not clone {source} at {ref}", file=sys.stderr)
    if clone is None:
        fail(f"{component}: no source produced {ref}")
    head = subprocess.run(
        ["git", "-C", str(destination), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if head != commit:
        shutil.rmtree(destination, ignore_errors=True)
        fail(f"{component}: {ref} is {head}, {VERSIONS_PATH} pins {commit}")


def _fetch_archive(component, pins, destination):
    url, expected = pins.get("url"), pins.get("sha256")
    if not url:
        fail(f"{VERSIONS_PATH}: {component} has no url to download")
    if not expected:
        fail(f"{VERSIONS_PATH}: {component} has no sha256 to verify the archive against")
    # Same source policy as the clone path: the primary source first, the
    # optional mirror only once it has failed, and the same digest demanded of
    # either. The 30s socket timeout matches the linux driver's own curl
    # --connect-timeout.
    sources = [url] + ([pins["mirror"]] if pins.get("mirror") else [])

    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Unpack into a staging directory beside the destination -- same
    # filesystem, so the move at the end is a rename -- and publish it as
    # `destination` only once every member has landed. A failed fetch must not
    # leave a half-tree behind for the next run's "already exists" guard.
    staging = Path(
        tempfile.mkdtemp(dir=destination.parent, prefix=f".{destination.name}.staging.")
    )
    installed = False
    try:
        with tempfile.TemporaryDirectory(dir=".") as work:
            archive = Path(work) / "archive"
            fetched = False
            for source in sources:
                for attempt in range(1, 4):
                    try:
                        with (
                            urllib.request.urlopen(source, timeout=30) as response,
                            archive.open("wb") as handle,
                        ):
                            shutil.copyfileobj(response, handle)
                    except (OSError, http.client.HTTPException, ValueError) as error:
                        print(f"could not download {source}: {error}", file=sys.stderr)
                        archive.unlink(missing_ok=True)
                        if attempt < 3:
                            time.sleep(attempt)
                        continue
                    digest = sha256_file(archive)
                    if digest == expected:
                        fetched = True
                        break
                    # A mismatch is not a transient failure like a dropped
                    # connection, but a retry covers a truncated transfer and
                    # the mirror is the only other candidate for these bytes.
                    print(
                        f"{source}: sha256 {digest}, {VERSIONS_PATH} pins {expected}",
                        file=sys.stderr,
                    )
                    archive.unlink(missing_ok=True)
                    if attempt < 3:
                        time.sleep(attempt)
                if fetched:
                    break
            if not fetched:
                fail(f"{component}: no source served the pinned archive")

            try:
                with tarfile.open(archive) as tar:
                    tar.extractall(
                        staging, members=_stripped_members(component, tar), filter="data"
                    )
            except Exception as error:
                # Anything that makes the bytes unusable -- tar structure, the
                # decompressor, the filesystem -- is a fetch failure, not a
                # traceback. `fail` raises SystemExit, which passes through.
                fail(f"{component}: cannot extract the downloaded archive: {error}")

        os.replace(staging, destination)
        installed = True
    finally:
        if not installed:
            shutil.rmtree(staging, ignore_errors=True)
            shutil.rmtree(destination, ignore_errors=True)


def _stripped_members(component, tar):
    """The archive's members with their one top-level component dropped.

    An archive pin is an upstream release tarball whose every member sits
    under a single top-level directory (ffmpeg's `ffmpeg-7.1/`, mpv's
    `mpv-0.40.0/`), and that directory is the source tree the drivers build,
    so exactly one top-level component has to be there to drop. A flat
    tarball, several roots, or an absolute/`..`/`.` root would otherwise
    extract the wrong tree, or nothing at all, while the fetch still reported
    success.
    """
    members = tar.getmembers()
    roots = sorted({Path(member.name).parts[0] for member in members if Path(member.name).parts})
    if len(roots) != 1:
        fail(
            f"{component}: archive has {len(roots)} top-level components "
            f"({', '.join(roots) or 'none'}); exactly one is required to strip it"
        )
    # An absolute member name's first part is the anchor `"/"`, not a directory,
    # so it passes the count and the `.`/`..` test and then strips to the tree
    # below it: a tar rooted at `/etc` would publish `etc/...` as the component.
    if roots[0] in (".", "..") or Path(roots[0]).is_absolute():
        fail(f"{component}: archive's only top-level component is {roots[0]!r}; cannot strip it")

    renamed = {}
    stripped = []
    for member in members:
        parts = Path(member.name).parts[1:]
        if not parts:
            # The root directory's own entry ("mpv-0.40.0/"): it is the one
            # member the strip legitimately reduces to nothing.
            if member.isdir():
                continue
            fail(f"{component}: archive member {member.name!r} is the whole archive")
        if Path(*parts).is_absolute() or ".." in parts:
            fail(f"{component}: archive member {member.name!r} escapes the extraction root")
        renamed[member.name] = str(Path(*parts))

    for member in members:
        if member.name not in renamed:
            continue
        # A hardlink names its target by the path it carried before the strip,
        # and tarfile resolves that name against the members it is handed, so
        # the rewrite has to follow it -- `tar --strip-components` does the
        # same. A symlink's linkname is relative to the member's own directory,
        # which the strip moves with it, so it is left alone.
        if member.islnk():
            target = renamed.get(member.linkname)
            if target is None:
                fail(
                    f"{component}: archive hardlink {member.name!r} points at "
                    f"{member.linkname!r}, which the strip does not carry"
                )
            member.linkname = target
        member.name = renamed[member.name]
        stripped.append(member)

    if not stripped:
        fail(f"{component}: archive holds no members to extract")
    return stripped


def group_components():
    """group -> the versions.json components that group's build consumes."""
    consumed = {}
    for path in sorted(Path("platforms").glob("*/group.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        names = set()
        for spec in (data.get("artifacts") or {}).values():
            names.update(spec.get("components") or ())
        consumed[path.parent.name] = names
    # The apple group is defined in the Swift driver rather than a group.json,
    # so what it builds is read out of that driver's own `builds:` list: every
    # library the group compiles, not just the ones keys.py publishes or the
    # ones a series file happens to touch. Deriving it from `series.apple` left
    # 16 of the 18 apple components unchecked, which is the "unread decoration"
    # this check exists to catch.
    consumed.setdefault("apple", set()).update(apple_driver_components())
    return consumed


def apple_driver_components():
    """The components the apple driver's build list names, canonicalized.

    Empty when the driver is not in the tree; `cmd_check` reports that rather
    than letting the apple group silently check nothing.
    """
    if not APPLE_DRIVER.is_file():
        return set()
    text = APPLE_DRIVER.read_text(encoding="utf-8")
    entries = _swift_array_entries(text, "let builds: [BaseBuild] = [")
    if entries is None:
        return set()
    libraries = _swift_class_libraries(text)
    names = set()
    for entry in entries:
        inline = re.search(r"library:\s*\.(\w+)", entry)
        if inline:
            names.add(inline.group(1))
            continue
        # `BuildASS()` names a subclass; the library it builds is the value its
        # own initializer hands to super.
        constructed = re.search(r"\b(\w+)\(\)", entry)
        if constructed and constructed.group(1) in libraries:
            names.add(libraries[constructed.group(1)])
    return {canonical_component(name) for name in names}


def _swift_array_entries(text, header):
    """The entries of the Swift array literal `header` opens, whitespace-stripped."""
    start = text.find(header)
    if start < 0:
        return None
    # Scan from just past the literal's opening bracket: the header carries the
    # element type in brackets of its own, so counting from the header itself
    # would close on that annotation.
    depth = 1
    for index in range(start + len(header), len(text)):
        if text[index] == "[":
            depth += 1
        elif text[index] == "]":
            depth -= 1
            if depth == 0:
                body = text[start + len(header) : index]
                return [line.strip() for line in body.splitlines() if line.strip()]
    return None


def _swift_class_libraries(text):
    """Swift class name -> the Library its initializer passes to super.init."""
    classes = list(re.finditer(r"^(?:private )?class (\w+)", text, re.M))
    libraries = {}
    for position, match in enumerate(classes):
        end = classes[position + 1].start() if position + 1 < len(classes) else len(text)
        inherited = re.search(r"super\.init\(library:\s*\.(\w+)\)", text[match.end() : end])
        if inherited:
            libraries[match.group(1)] = inherited.group(1)
    return libraries


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
    consumed = {group: names for group, names in group_components().items() if names}
    if VERSIONS_PATH.is_file():
        components = json.loads(VERSIONS_PATH.read_text(encoding="utf-8")).get("components", {})
        check_platform_declarations(components, problems)
    elif consumed:
        # The declaration check is the whole reason this file is read, so a
        # tree that builds components without pinning them is broken rather
        # than exempt. Reporting nothing here would let every `platforms`
        # array go unread behind a green run.
        problems.append(
            f"{VERSIONS_PATH}: missing; it is what declares the platforms that "
            f"{', '.join(sorted(consumed))} build their components for"
        )

    if Path("platforms").is_dir():
        # The apple group's only source of truth is the Swift driver's own
        # build list: it has no group.json. Missing or unreadable, the group
        # appears to build nothing and every apple component goes unchecked.
        if not APPLE_DRIVER.is_file():
            problems.append(
                f"{APPLE_DRIVER}: missing; the apple group's build list is what "
                f"its components are checked against"
            )
        elif not apple_driver_components():
            problems.append(
                f"{APPLE_DRIVER}: found no `let builds: [BaseBuild] = [...]` entries; "
                f"the apple group's components cannot be checked against it"
            )

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
