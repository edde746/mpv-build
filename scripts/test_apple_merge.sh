#!/usr/bin/env bash
# Regression test for scripts/merge_xcframeworks.py, the apple publish job's
# merge of the per-platform build legs: two merges of the same artifacts must
# write byte-identical zips.
#
# publish-assets refuses a rebuilt content key whose bytes differ from the
# asset already published under its name, and a key is rebuilt whenever a run
# uploads its assets but loses its manifest push. The merge used to zip with
# live mtimes, and xcodebuild writes the slices of a multi-platform
# xcframework's Info.plist in a different order on every run, so no merged
# zip could be reproduced (run 38394686727: Libavcodec-6b37176e1f15).
#
# The fixture is three tiny static frameworks (macOS with its versioned
# symlink layout, iOS, iOS simulator) built from one C file, so the real
# xcodebuild runs in seconds. The second merge runs later, under another time
# zone, from a fresh copy of the artifacts.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(uname -s)" != Darwin ] || ! xcrun --find xcodebuild >/dev/null 2>&1; then
  echo "SKIP: the apple merge needs macOS with Xcode"
  exit 0
fi

python3 - "$root" <<'PY'
import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
merge = root / "scripts" / "merge_xcframeworks.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def sh(*args, cwd=None, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True, stdout=subprocess.DEVNULL)


# (leg, sdk, target triple, versioned bundle) -- one leg per platform, as the
# publish workflow's build matrix uploads them.
LEGS = [
    ("apple-macos", "macosx", "arm64-apple-macos11.0", True),
    ("apple-iphoneos", "iphoneos", "arm64-apple-ios13.0", False),
    ("apple-iphonesimulator", "iphonesimulator", "arm64-apple-ios13.0-simulator", False),
]


def make_framework(directory: Path, sdk: str, target: str, versioned: bool) -> Path:
    source = directory / "probe.c"
    source.write_text("int probe(void) { return 1; }\n", encoding="utf-8")
    sh("xcrun", "-sdk", sdk, "clang", "-target", target, "-c", str(source), "-o", str(directory / "probe.o"))
    env = dict(os.environ, ZERO_AR_DATE="1")
    sh("xcrun", "libtool", "-static", "-o", str(directory / "libProbe.a"), str(directory / "probe.o"), env=env)

    framework = directory / "Probe.framework"
    content = framework / "Versions" / "A" if versioned else framework
    (content / "Headers").mkdir(parents=True)
    (content / "Headers" / "probe.h").write_text("int probe(void);\n", encoding="utf-8")
    shutil.copy2(directory / "libProbe.a", content / "Probe")
    info = {"CFBundleExecutable": "Probe", "CFBundleIdentifier": "test.Probe", "CFBundlePackageType": "FMWK"}
    if versioned:
        (content / "Resources").mkdir()
        (content / "Resources" / "Info.plist").write_bytes(plistlib.dumps(info))
        os.symlink("A", framework / "Versions" / "Current")
        for name in ("Probe", "Headers", "Resources"):
            os.symlink(f"Versions/Current/{name}", framework / name)
    else:
        (content / "Info.plist").write_bytes(plistlib.dumps(info))
    return framework


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    partials = tmp / "partials"
    for index, (leg, sdk, target, versioned) in enumerate(LEGS):
        work = tmp / "legs" / leg
        work.mkdir(parents=True)
        framework = make_framework(work, sdk, target, versioned)
        sh("xcodebuild", "-create-xcframework", "-framework", str(framework),
           "-output", str(work / "Probe.xcframework"))
        out = partials / leg
        out.mkdir(parents=True)
        # Inputs zipped with live metadata on purpose: the merge has to
        # normalize whatever the legs hand it.
        sh("/usr/bin/zip", "-qry", str(out / "Probe.xcframework.zip"), "Probe.xcframework", cwd=work)
        (out / f"Probe-all-0123456789ab-leg{index}.zip").write_bytes(f"prebuilt {leg}\n".encode())

    runs = []
    for name, tz, umask in (("first", "UTC", 0o022), ("second", "Pacific/Auckland", 0o022)):
        work = tmp / name
        work.mkdir()
        shutil.copytree(partials, work / "partials", symlinks=True)
        previous = os.umask(umask)
        try:
            # The workflow's invocation, relative paths and all.
            result = subprocess.run(
                [sys.executable, str(merge), "partials", "release"], cwd=work,
                env=dict(os.environ, TZ=tz), capture_output=True, text=True,
            )
        finally:
            os.umask(previous)
        check(result.returncode == 0, f"{name} merge failed:\n{result.stdout}\n{result.stderr}")
        runs.append(work / "release")
        # zip stores 2-second DOS times: make sure the clock moved.
        time.sleep(2.1)

    first, second = runs
    merged = first / "Probe.xcframework.zip"
    check(merged.is_file(), "the merge writes Probe.xcframework.zip")
    produced = sorted(path.name for path in first.glob("*.zip"))
    check(
        produced == sorted(path.name for path in second.glob("*.zip")),
        f"both merges write the same zips: {produced}",
    )
    identical = 0
    for name in produced:
        a, b = first / name, second / name
        if a.read_bytes() == b.read_bytes():
            identical += 1
            continue
        # Name what moved, so a failure points at its input.
        with zipfile.ZipFile(a) as left, zipfile.ZipFile(b) as right:
            fields = ("filename", "CRC", "external_attr", "date_time", "extra")
            rows_a = [tuple(getattr(entry, field) for field in fields) for entry in left.infolist()]
            rows_b = [tuple(getattr(entry, field) for field in fields) for entry in right.infolist()]
        moved = [
            f"{row_a[0]}: " + ", ".join(
                f"{field} {value_a!r} != {value_b!r}"
                for field, value_a, value_b in zip(fields, row_a, row_b)
                if value_a != value_b
            )
            for row_a, row_b in zip(rows_a, rows_b)
            if row_a != row_b
        ]
        if len(rows_a) != len(rows_b):
            moved.append(f"{len(rows_a)} entries != {len(rows_b)} entries")
        check(False, f"{name}: two merges of the same artifacts wrote different bytes:\n  " + "\n  ".join(moved[:8]))
    for index, (leg, *_rest) in enumerate(LEGS):
        name = f"Probe-all-0123456789ab-leg{index}.zip"
        check(
            (first / name).read_bytes() == (partials / leg / name).read_bytes(),
            f"{name}: a prebuilt zip must pass through unchanged",
        )
    print(f"two merges, different clock and TZ: {identical} of {len(produced)} zips byte-identical")

    with zipfile.ZipFile(merged) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        check(names == sorted(names), "merged zip entries are in sorted order")
        check(not any(name.endswith("/") for name in names), "merged zip stores no directory entries")
        check({entry.date_time for entry in entries} == {(2020, 1, 1, 0, 0, 0)}, "merged zip mtimes are normalized")
        check({entry.extra for entry in entries} == {b""}, "merged zip stores no extra fields (uid/gid, UT times)")
        links = {
            entry.filename: archive.read(entry).decode()
            for entry in entries
            if stat.S_ISLNK(entry.external_attr >> 16)
        }
        check(
            links.get("Probe.xcframework/macos-arm64/Probe.framework/Versions/Current") == "A",
            f"the macOS bundle's symlinks survive the merge: {sorted(links)}",
        )
        info = plistlib.loads(archive.read("Probe.xcframework/Info.plist"))
    identifiers = [library["LibraryIdentifier"] for library in info["AvailableLibraries"]]
    check(
        identifiers == ["ios-arm64", "ios-arm64-simulator", "macos-arm64"],
        f"AvailableLibraries sorted by LibraryIdentifier: {identifiers}",
    )
    print("merged zip: sorted entries, fixed metadata, symlinks kept, slices sorted")

# The merge must stay an apple key input: a change to how the merged bytes are
# made has to move the key, not collide with what is already published.
# keys.py imports its sibling patches.py, so scripts/ goes first on the path.
sys.path.insert(0, str(root / "scripts"))
import keys  # noqa: E402

covering = [str(path) for path in keys.GROUPS["apple"].driver_sources if path.name == merge.name]
check(bool(covering), f"{merge.name} must be an apple driver source (a key input)")
for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
    check(
        "create-xcframework" not in workflow.read_text(encoding="utf-8"),
        f"{workflow.name}: xcframeworks are merged by {merge.name}, not a workflow script",
    )
print(f"the merge is keyed through {', '.join(covering) or 'nothing'}; no workflow builds xcframeworks itself")

if failures:
    print(f"{len(failures)} failure(s)")
    sys.exit(1)
print("apple merge: OK")
PY
