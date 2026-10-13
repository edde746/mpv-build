#!/usr/bin/env python3
"""Merge the per-platform apple build artifacts into the release directory.

Usage: merge_xcframeworks.py <partials dir> <release dir>

<partials dir> holds one subdirectory per platform build (the apple-<platform>
workflow artifacts). Their per-platform `*-all-*.zip` prebuilt zips pass
straight through: they are the artifacts a later run restores instead of
recompiling, and the driver already zips them reproducibly and names them by
content. Every `<Name>.xcframework.zip` is unpacked from all platforms and
merged into one multi-platform XCFramework with `xcodebuild
-create-xcframework`, then zipped into <release dir>.

The merged zip becomes the content-addressed `<Name>-<key>.xcframework.zip`
release asset, and publish-assets requires a rebuild of an already published
key to reproduce its bytes. Two merges of identical inputs differed twice
over: a plain `zip -r` stored the wall-clock mtimes create-xcframework gives
every directory and Info.plist, and create-xcframework lists the platform
slices in Info.plist's AvailableLibraries in a different order on every run.
So a run that rebuilt a key whose assets an earlier run had published (its
manifest commit lost a push race) failed on Libavcodec. The slices are now
sorted by LibraryIdentifier, and the zip is made the way the driver's
deterministicZip makes the per-platform ones: every mtime set to 2020-01-01,
regular files and symlinks only, in byte order, no extra attributes (-X),
symlinks kept (-y). This file produces keyed bytes, so it is one of the apple
group's driver sources in keys.py: changing it moves every apple key.
"""

from __future__ import annotations

import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# 2020-01-01 00:00 local time, the stamp base.swift's deterministicZip uses
# (`touch -t 202001010000`); zip stores the local DOS time.
NORMALIZED_MTIME = "202001010000"


def canonicalize_info_plist(xcframework: Path) -> None:
    info = xcframework / "Info.plist"
    with info.open("rb") as handle:
        plist = plistlib.load(handle)
    plist["AvailableLibraries"].sort(key=lambda library: library["LibraryIdentifier"])
    # Rewritten in place, so the file keeps the mode create-xcframework gave it.
    with info.open("r+b") as handle:
        handle.truncate()
        plistlib.dump(plist, handle, fmt=plistlib.FMT_XML, sort_keys=True)


def deterministic_zip(entry: str, zip_file: Path, cwd: Path) -> None:
    subprocess.run(["/usr/bin/find", entry, "-exec", "/usr/bin/touch", "-h", "-t", NORMALIZED_MTIME, "{}", "+"],
                   cwd=cwd, check=True)
    members = []
    for directory, dirnames, filenames in os.walk(cwd / entry):
        base = Path(directory)
        for name in [*filenames, *dirnames]:
            path = base / name
            # os.walk lists a symlink to a directory under dirnames and does not
            # descend into it; plain directories are not zip members.
            if path.is_symlink() or path.is_file():
                members.append(os.fsencode(path.relative_to(cwd)))
    members.sort()
    subprocess.run(["/usr/bin/zip", "-q", "-X", "-y", str(zip_file.resolve()), "-@"],
                   cwd=cwd, input=b"\n".join(members) + b"\n", check=True)


def merge(partial_root: Path, release_dir: Path) -> None:
    xcframework_dir = release_dir / "xcframework"
    if release_dir.exists():
        shutil.rmtree(release_dir)
    xcframework_dir.mkdir(parents=True)

    prebuilt_zips = sorted(partial_root.glob("*/*-all-*.zip"))
    for zip_path in prebuilt_zips:
        shutil.copy2(zip_path, release_dir / zip_path.name)
    print(f"Collected {len(prebuilt_zips)} per-platform prebuilt zips")

    grouped_zips: dict[str, list[Path]] = {}
    for zip_path in partial_root.glob("*/*.xcframework.zip"):
        grouped_zips.setdefault(zip_path.name, []).append(zip_path)
    if not grouped_zips:
        raise SystemExit("No platform XCFramework artifacts found")

    for zip_name, zip_parts in sorted(grouped_zips.items()):
        framework_name = zip_name[:-len(".xcframework.zip")]
        output_xcframework = xcframework_dir / f"{framework_name}.xcframework"
        framework_paths = []

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            for index, zip_part in enumerate(sorted(zip_parts)):
                extract_dir = tmp_path / f"part-{index}"
                extract_dir.mkdir()
                subprocess.run(["/usr/bin/unzip", "-q", str(zip_part.resolve()), "-d", str(extract_dir)], check=True)
                framework_paths.extend(sorted(path for path in extract_dir.glob("*.xcframework/**/*.framework")
                                              if path.is_dir()))

            if not framework_paths:
                raise SystemExit(f"No frameworks found inside {zip_name}")

            args = ["/usr/bin/xcodebuild", "-create-xcframework"]
            for framework_path in framework_paths:
                args.extend(["-framework", str(framework_path)])
            args.extend(["-output", str(output_xcframework)])
            subprocess.run(args, check=True)

        canonicalize_info_plist(output_xcframework)
        deterministic_zip(output_xcframework.name, release_dir / zip_name, xcframework_dir)
        print(f"Merged {zip_name} from {len(zip_parts)} platform artifacts")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        raise SystemExit(__doc__.strip().splitlines()[2])
    merge(Path(argv[1]), Path(argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
