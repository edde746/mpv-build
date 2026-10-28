#!/usr/bin/env python3
"""Licence texts of the components linked into each published artifact.

Usage:
  licenses.py collect <licenses dir> [--group <group>] <component>=<source dir>...
  licenses.py check <licenses dir> [--group <group>] [<component>...]

Every artifact carries `licenses/<component>/`: the licence texts of each
component linked into it, copied unchanged from the source tree the build
compiled, at the path they have in that tree. GPL, LGPL, BSD, MIT and the
FreeType licence all ask for their text to go with every binary copy.

`collect` copies, for each <component>=<source dir>, into
<licenses dir>/<component>/ (replacing what was there):

  - every file at the root of the source tree named like a licence
    (COPYING*, LICENSE*, LICENCE*, COPYRIGHT*, Copyright, NOTICE*, PATENTS*,
    UNLICENSE*), and every file under a root directory named that way
    (LICENSES/), unless RULES turns the root off; a git tree's root files
    come from its HEAD where a sparse checkout left them out;
  - what RULES adds for the component: the same root files of vendored
    subtrees the build compiles in (`trees`), named files (`files`, globs),
    licence comments cut out of source files from the first line holding
    `start` to the next line ending in `end`, both included, written as <file>.txt
    (`excerpts`, for code whose only licence text is a comment), and the
    licence files of every crate in the normal dependency graph of a Cargo
    manifest, under crates/<name>-<version>/ (`cargo`).

A component, a tree, a file glob, an excerpt or a crate that yields no text
fails the run: a version bump that moves a licence file stops the build
instead of shipping a binary without its text. Files are written with mode
0644 so the archives stay byte-reproducible.

`check` fails unless <licenses dir> holds exactly one folder per expected
component, each with at least one non-empty file: the components of the
group's multi-component artifact in platforms/<group>/group.json (the ones its
key hashes, which is the set the artifact links), plus any named. Packaging
runs it on the staged artifact.

This file decides bytes of every artifact, so every group's driver lists it
and its key moves with it.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

LICENCE_NAME = re.compile(r"(copying|licen[cs]es?|copyright|notice|patents|unlicense)([._-].*)?", re.IGNORECASE)
# Build and packaging files that happen to start like a licence name.
NOT_TEXT = {".py", ".sh", ".cmake", ".in", ".am", ".ac", ".bat", ".ps1", ".pl", ".c", ".cc", ".cpp", ".h", ".m",
            ".rs", ".swift", ".toml", ".json", ".yml", ".yaml", ".cfg", ".gn", ".bzl", ".bazel"}


@dataclass(frozen=True)
class Rule:
    # The licence files at the source root; off where the root holds texts of things not linked.
    root: bool = True
    # Vendored subtrees the build compiles in, each searched like the root.
    trees: tuple[str, ...] = ()
    # Further licence files, as globs relative to the source root.
    files: tuple[str, ...] = ()
    # (glob, start, end): the lines from the first holding `start` to the next holding `end`.
    excerpts: tuple[tuple[str, str, str], ...] = ()
    # (Cargo.toml, features): the licence files of the crates the manifest's library links.
    cargo: tuple[str, str] | None = None


MIT_END = "DEALINGS IN THE SOFTWARE."

RULES = {
    # Its root LICENSE.TXT names these: the FreeType licence and the GPLv2 it is offered under, the X11-style
    # licences of the BDF and PCF drivers and of fthash.c, the zlib licence of the gzip module, and the HarfBuzz
    # code in the autofitter.
    "freetype": Rule(
        files=("docs/FTL.TXT", "docs/GPLv2.TXT", "src/bdf/README", "src/pcf/README"),
        excerpts=(
            ("src/base/fthash.c", "Copyright 2000", MIT_END),
            ("src/gzip/zlib.h", "Copyright (C) 1995-", "madler@alumni.caltech.edu"),
            ("src/autofit/ft-hb-ft.c", "Copyright", "MODIFICATIONS."),
        ),
    ),
    # The Universal Shaping Engine data it compiles in is under its own MIT licence.
    "harfbuzz": Rule(files=("src/ms-use/COPYING",)),
    # The Lua 5.2 tarball has no licence file; lua.h ends with the licence.
    "lua": Rule(excerpts=(("src/lua.h", "* Copyright (C) 1994-", MIT_END),)),
    # Headers only, each under the MIT licence in its leading comment; no licence file.
    "nv-codec-headers": Rule(excerpts=(("include/ffnvcodec/*.h", "Copyright", MIT_END),)),
    # The SPIR-V compilers shaderc's libraries carry (utils/git-sync-deps fetches them).
    "shaderc": Rule(trees=("third_party/glslang", "third_party/spirv-tools", "third_party/spirv-headers")),
    # fast_float is compiled in; glad generates the OpenGL loader and Vulkan-Headers' registry the Vulkan code from
    # Khronos' registries.
    "libplacebo": Rule(trees=("3rdparty/fast_float", "3rdparty/glad", "3rdparty/Vulkan-Headers")),
    # dovi_tool's library crate and the crates it links.
    "libdovi": Rule(trees=("dolby_vision",), cargo=("dolby_vision/Cargo.toml", "capi")),
    # The NDK's libc++_shared.so: the LLVM licence at the head of the toolchain's NOTICE, whose rest is the
    # licences of the toolchain's other parts. The source is the NDK's toolchains/llvm/prebuilt/<host>.
    "libc++": Rule(
        root=False,
        excerpts=(("NOTICE", "The LLVM Project is under the Apache License v2.0 with LLVM Exceptions:", "Software."),),
    ),
    # The Rust standard library every Rust static library carries, with the notices of the crates it is built
    # from. The source is the pinned toolchain's share/doc/rust.
    "rust-std": Rule(root=False, files=("COPYRIGHT-library.html",)),
}

# Rules a group's sources need instead (`collect --group`).
GROUP_RULES: dict[str, dict[str, Rule]] = {}


def fail(message: str) -> None:
    print(f"licenses.py: {message}", file=sys.stderr)
    raise SystemExit(1)


def write(target: Path, data: bytes) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(0o644)


def licence_files(tree: Path) -> list[tuple[Path, bytes]]:
    """(path in `tree`, bytes) of the licence files at its root and under its licence-named directories.

    A git tree's root files also come from its HEAD when the checkout left them out (a sparse checkout of the
    headers alone).
    """
    found = []
    for entry in sorted(tree.iterdir()):
        if not LICENCE_NAME.fullmatch(entry.name) or entry.suffix.lower() in NOT_TEXT:
            continue
        if entry.is_file():
            found.append((entry.relative_to(tree), entry.read_bytes()))
        elif entry.is_dir():
            found += [(path.relative_to(tree), path.read_bytes()) for path in sorted(entry.rglob("*")) if path.is_file()]
    if (tree / ".git").exists():
        listing = subprocess.run(["git", "-C", str(tree), "ls-tree", "-z", "HEAD"], check=True, capture_output=True)
        for record in listing.stdout.split(b"\0"):
            meta, _, name = record.decode().partition("\t")
            if (not name or meta.split()[1] != "blob" or (tree / name).exists()
                    or not LICENCE_NAME.fullmatch(name) or Path(name).suffix.lower() in NOT_TEXT):
                continue
            blob = subprocess.run(["git", "-C", str(tree), "show", f"HEAD:{name}"], check=True, capture_output=True)
            found.append((Path(name), blob.stdout))
    return sorted(item for item in found if item[1])


def excerpt(path: Path, start: str, end: str) -> bytes:
    lines = path.read_bytes().splitlines(keepends=True)
    first = next((i for i, line in enumerate(lines) if start.encode() in line), None)
    if first is None:
        fail(f"{path}: no line holds {start!r}")
    last = next((i for i in range(first, len(lines)) if lines[i].rstrip().endswith(end.encode())), None)
    if last is None:
        fail(f"{path}: no line after {start!r} ends in {end!r}")
    return b"".join(lines[first:last + 1])


def crates(source: Path, manifest: str, features: str) -> list[tuple[str, Path]]:
    """(name-version, crate directory) of every crate the manifest's package links, itself excluded.

    Normal dependencies only, followed from the package: build scripts, proc macros' own dependencies and
    dev-dependencies link nothing into the library. Every platform's dependencies count, so the set is the same
    for each target.
    """
    command = ["cargo", "metadata", "--locked", "--format-version", "1",
               "--manifest-path", str(source / manifest), "--features", features]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    metadata = json.loads(result.stdout)
    packages = {package["id"]: package for package in metadata["packages"]}
    nodes = {node["id"]: node for node in metadata["resolve"]["nodes"]}
    root = metadata["resolve"]["root"]
    if root is None:
        fail(f"{manifest}: cargo resolved no root package")
    seen, pending = set(), [root]
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        pending += [dep["pkg"] for dep in nodes[current]["deps"]
                    if any(kind["kind"] is None for kind in dep["dep_kinds"])]
    seen.discard(root)
    return sorted((f"{packages[id]['name']}-{packages[id]['version']}",
                   Path(packages[id]["manifest_path"]).parent) for id in seen)


def collect(component: str, source: Path, out: Path, group: str | None) -> int:
    if not source.is_dir():
        fail(f"{component}: no source tree at {source}")
    rule = GROUP_RULES.get(group or "", {}).get(component) or RULES.get(component, Rule())
    target = out / component
    shutil.rmtree(target, ignore_errors=True)
    written = 0

    def emit(path: Path, data: bytes) -> None:
        nonlocal written
        write(target / path, data)
        written += 1

    if rule.root:
        for path, data in licence_files(source):
            emit(path, data)
    for tree in rule.trees:
        found = licence_files(source / tree) if (source / tree).is_dir() else []
        if not found:
            fail(f"{component}: no licence file in {tree}")
        for path, data in found:
            emit(Path(tree) / path, data)
    for pattern in rule.files:
        matches = sorted(path for path in source.glob(pattern) if path.is_file() and path.stat().st_size > 0)
        if not matches:
            fail(f"{component}: nothing matches {pattern}")
        for path in matches:
            emit(path.relative_to(source), path.read_bytes())
    for pattern, start, end in rule.excerpts:
        matches = sorted(path for path in source.glob(pattern) if path.is_file())
        if not matches:
            fail(f"{component}: nothing matches {pattern}")
        for path in matches:
            relative = path.relative_to(source)
            emit(relative.with_name(relative.name + ".txt"), excerpt(path, start, end))
    if rule.cargo is not None:
        for name, crate in crates(source, *rule.cargo):
            found = licence_files(crate)
            if not found:
                fail(f"{component}: crate {name} ships no licence file")
            for path, data in found:
                emit(Path("crates") / name / path, data)
    if written == 0:
        fail(f"{component}: no licence text in {source}")
    return written


def group_components(group: str) -> list[str]:
    path = Path(__file__).resolve().parent.parent / "platforms" / group / "group.json"
    artifacts = json.loads(path.read_text(encoding="utf-8"))["artifacts"]
    linked = [spec["components"] for spec in artifacts.values() if "components" in spec]
    if len(linked) != 1:
        fail(f"{path}: expected one artifact with components, found {len(linked)}")
    return linked[0]


def check(out: Path, expected: set[str]) -> None:
    present = {entry.name for entry in out.iterdir() if entry.is_dir()} if out.is_dir() else set()
    problems = [f"no licence texts for {name}" for name in sorted(expected - present)]
    problems += [f"licence texts for {name}, which the artifact does not list" for name in sorted(present - expected)]
    for name in sorted(present & expected):
        if not any(path.is_file() and path.stat().st_size > 0 for path in (out / name).rglob("*")):
            problems.append(f"{out / name} holds no licence text")
    if problems:
        fail(f"{out}:\n  " + "\n  ".join(problems))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)
    collect_parser = subparsers.add_parser("collect", help="copy each component's licence texts")
    collect_parser.add_argument("out", type=Path)
    collect_parser.add_argument("--group", help="apply the group's GROUP_RULES")
    collect_parser.add_argument("sources", nargs="+", metavar="component=source")
    check_parser = subparsers.add_parser("check", help="fail unless every linked component has its texts")
    check_parser.add_argument("out", type=Path)
    check_parser.add_argument("--group")
    check_parser.add_argument("components", nargs="*")
    args = parser.parse_args(argv)

    if args.command == "collect":
        for pair in args.sources:
            component, separator, source = pair.partition("=")
            if not separator or not component or not source:
                fail(f"expected <component>=<source dir>, got {pair!r}")
            count = collect(component, Path(source), args.out, args.group)
            print(f"{component}: {count} licence file(s)")
        return 0
    expected = set(args.components)
    if args.group:
        expected |= set(group_components(args.group))
    if not expected:
        fail("check: no components to expect")
    check(args.out, expected)
    print(f"{args.out}: licence texts for {len(expected)} components")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
