#!/usr/bin/env bash
# Regression test for scripts/patches.py, the shared series-based patch
# framework: resolved order is authoritative, `check` refuses inconsistent
# trees, and `apply` drives `git apply` in series order.
#
# Scenarios run against synthetic trees in a temporary directory, so the
# assertions can be exact (which order, which failure) without touching the
# network. One scenario runs against this repository itself, so the real
# pools and series files stay covered.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$root" <<'PY'
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
tool = root / "scripts" / "patches.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(cwd, *args, expect=0):
    result = subprocess.run(
        [sys.executable, str(tool), *args],
        capture_output=True,
        text=True,
        cwd=cwd,
    )
    if expect is not None and result.returncode != expect:
        failures.append(
            f"{' '.join(args)} exited {result.returncode}, expected {expect}\n"
            f"{result.stdout}{result.stderr}"
        )
        print(f"FAIL: {' '.join(args)} exited {result.returncode}, expected {expect}")
        print(result.stdout, result.stderr)
    return result


def make_tree(tmp):
    """A widget component whose series order is deliberately not sorted."""
    pool = Path(tmp) / "patches" / "widget" / "pool"
    pool.mkdir(parents=True)
    for name in ("0001-common.patch", "0002-zeta.patch", "0003-alpha.patch"):
        (pool / name).write_text(f"fixture bytes of {name}\n", encoding="utf-8")
    component = pool.parent
    (component / "series.common").write_text(
        "# common comes first\n0001-common.patch\n", encoding="utf-8"
    )
    (component / "series.apple").write_text(
        "# zeta before alpha: order is authoritative, never lexicographic\n"
        "0002-zeta.patch\n"
        "\n"
        "0003-alpha.patch\n",
        encoding="utf-8",
    )
    return component


# 1. resolve: series.common first, then series.<platform>, in file order.
with tempfile.TemporaryDirectory() as tmp:
    make_tree(tmp)
    result = run(tmp, "resolve", "widget", "apple")
    check(
        result.stdout.split() == ["0001-common.patch", "0002-zeta.patch", "0003-alpha.patch"],
        f"resolve order must be common then apple, in file order; got {result.stdout.split()}",
    )
    result = run(tmp, "resolve", "widget", "android")
    check(
        result.stdout.split() == ["0001-common.patch"],
        "a platform without a series file resolves to series.common alone",
    )
    result = run(tmp, "resolve", "nonexistent", "apple")
    check(result.stdout == "", "a component without a patches directory resolves empty")
    run(tmp, "resolve", "widget", "macos", expect=1)
    print("resolve honors series order")

# 2. check: pass on a clean tree, fail on each contract violation.
with tempfile.TemporaryDirectory() as tmp:
    component = make_tree(tmp)
    run(tmp, "check")

    # a pool file no series references
    orphan = component / "pool" / "0004-orphan.patch"
    orphan.write_text("orphan\n", encoding="utf-8")
    result = run(tmp, "check", expect=1)
    check("0004-orphan.patch" in result.stderr, "check must name the unreferenced pool file")
    orphan.unlink()

    # a series entry with no pool file
    series = component / "series.apple"
    original = series.read_text(encoding="utf-8")
    series.write_text(original + "0005-missing.patch\n", encoding="utf-8")
    result = run(tmp, "check", expect=1)
    check("0005-missing.patch" in result.stderr, "check must name the missing pool file")

    # the same entry twice in one resolved series (common + platform)
    series.write_text(original + "0001-common.patch\n", encoding="utf-8")
    result = run(tmp, "check", expect=1)
    check(
        "twice" in result.stderr and "0001-common.patch" in result.stderr,
        "check must flag a duplicate entry in one resolved series",
    )
    series.write_text(original, encoding="utf-8")
    run(tmp, "check")
    print("check accepts the clean tree and rejects each violation")

# 4. apply / apply --check against a tiny git repository.
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "src"
    src.mkdir()
    env = dict(os.environ)
    subprocess.run(["git", "init", "-q"], cwd=src, check=True, env=env)
    (src / "greeting.txt").write_text("hello\n", encoding="utf-8")

    pool = Path(tmp) / "patches" / "widget" / "pool"
    pool.mkdir(parents=True)
    (pool.parent / "series.apple").write_text("0001-greeting.patch\n", encoding="utf-8")
    (pool / "0001-greeting.patch").write_text(
        "--- a/greeting.txt\n"
        "+++ b/greeting.txt\n"
        "@@ -1 +1 @@\n"
        "-hello\n"
        "+goodbye\n",
        encoding="utf-8",
    )

    run(tmp, "apply", "widget", "apple", str(src), "--check")
    check(
        (src / "greeting.txt").read_text(encoding="utf-8") == "hello\n",
        "apply --check must not modify the source tree",
    )
    run(tmp, "apply", "widget", "apple", str(src))
    check(
        (src / "greeting.txt").read_text(encoding="utf-8") == "goodbye\n",
        "apply must apply the resolved series",
    )
    # now the patch no longer applies: fail fast, name the patch
    result = run(tmp, "apply", "widget", "apple", str(src), "--check", expect=1)
    check(
        "0001-greeting.patch" in result.stderr,
        "a patch that does not apply must fail the run and be named",
    )
    print("apply and apply --check drive git apply in series order")

# 4b. apply --check must be sound for stacked series: a later patch may build
# on an earlier patch's hunks, and a mid-series failure must unwind cleanly.
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "src"
    src.mkdir()
    subprocess.run(["git", "init", "-q"], cwd=src, check=True)
    (src / "greeting.txt").write_text("hello\n", encoding="utf-8")

    pool = Path(tmp) / "patches" / "widget" / "pool"
    pool.mkdir(parents=True)
    series = pool.parent / "series.apple"
    series.write_text("0001-first.patch\n0002-stacked.patch\n", encoding="utf-8")
    (pool / "0001-first.patch").write_text(
        "--- a/greeting.txt\n+++ b/greeting.txt\n@@ -1 +1 @@\n-hello\n+goodbye\n",
        encoding="utf-8",
    )
    # applies only after 0001: its context line is 0001's output
    (pool / "0002-stacked.patch").write_text(
        "--- a/greeting.txt\n+++ b/greeting.txt\n@@ -1 +1,2 @@\n goodbye\n+world\n",
        encoding="utf-8",
    )

    run(tmp, "apply", "widget", "apple", str(src), "--check")
    check(
        (src / "greeting.txt").read_text(encoding="utf-8") == "hello\n",
        "a stacked series must pass --check and leave the tree untouched",
    )

    # a failing tail patch: the applied prefix must be unwound
    (pool / "0003-bad.patch").write_text(
        "--- a/greeting.txt\n+++ b/greeting.txt\n@@ -1 +1 @@\n-no-such-line\n+x\n",
        encoding="utf-8",
    )
    series.write_text(
        "0001-first.patch\n0002-stacked.patch\n0003-bad.patch\n", encoding="utf-8"
    )
    result = run(tmp, "apply", "widget", "apple", str(src), "--check", expect=1)
    check("0003-bad.patch" in result.stderr, "the failing patch must be named")
    check(
        (src / "greeting.txt").read_text(encoding="utf-8") == "hello\n",
        "a failed --check must unwind the applied prefix",
    )
    print("apply --check is sound for stacked series and unwinds on failure")

# 4c. fetch-pinned's archive path. The strip has to leave exactly the source
# tree the drivers build, and every way of getting that wrong has to fail loudly
# rather than publish the wrong tree as a fetched success. These run against
# `file://` pins, so no network and no retry sleeps: every case below fails
# after the download, in the strip.
def make_archive(path, rows):
    """A tarball from (name, kind, payload) rows, written in order.

    kind is "dir", "file", "link" (hardlink, payload = target name) or
    "symlink" (payload = target). Built through tarfile rather than the host
    `tar` so the member names -- absolute ones included -- are exactly as
    written, and no platform adds its own members.
    """
    with tarfile.open(path, "w") as tar:
        for name, kind, payload in rows:
            info = tarfile.TarInfo(name)
            if kind == "dir":
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            elif kind in ("link", "symlink"):
                info.type = tarfile.LNKTYPE if kind == "link" else tarfile.SYMTYPE
                info.linkname = payload
                tar.addfile(info)
            else:
                data = payload.encode()
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    return path


def fetch_archive(case, rows, expect=0, needles=(), expect_tree=None):
    """Materialise one archive pin in a sandbox and assert what it left behind.

    `expect_tree` runs inside the sandbox -- while the extracted tree still
    exists -- and returns (condition, message) pairs to check.
    """
    with tempfile.TemporaryDirectory() as tmp:
        sandbox = Path(tmp)
        (sandbox / "patches" / "widget").mkdir(parents=True)
        (sandbox / "patches" / "widget" / "series.apple").write_text("", encoding="utf-8")
        archive = make_archive(sandbox / "widget-1.0.tar.gz", rows)
        (sandbox / "versions.json").write_text(
            json.dumps(
                {
                    "components": {
                        "widget": {
                            "kind": "archive",
                            "version": "1.0",
                            "url": archive.as_uri(),
                            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                            "platforms": ["apple"],
                        }
                    }
                }
            ),
            encoding="utf-8",
        )
        destination = sandbox / "out" / "widget"
        result = run(sandbox, "fetch-pinned", "widget", "apple", str(destination), expect=expect)
        for needle in needles:
            check(
                needle in result.stderr,
                f"{case}: expected {needle!r} in the error, got {result.stderr.strip()!r}",
            )
        if expect != 0:
            check(
                not destination.exists(),
                f"{case}: a refused archive must not publish {destination.name}",
            )
        elif expect_tree is not None:
            for condition, message in expect_tree(destination):
                check(condition, f"{case}: {message}")


def single_root(destination):
    """The root directory's own entry is the one member the strip reduces to
    nothing; the tree below it is what the drivers build."""
    return [
        (
            (destination / "greeting.txt").read_text(encoding="utf-8") == "hello\n"
            and (destination / "deep/nested.txt").read_text(encoding="utf-8") == "nested\n",
            "a single-rooted archive must extract its tree with the top directory stripped",
        ),
        (
            not (destination / "widget-1.0").exists(),
            "the stripped top-level directory must not survive as a member",
        ),
    ]


# A hardlink names its target by its pre-strip path, and tarfile resolves that
# name against the members it is handed, so the strip has to rewrite it.
# Otherwise the fetch dies with `linkname 'widget-1.0/greeting.txt' not found`.
def hardlink(destination):
    link = destination / "greeting.link"
    return [
        (
            link.exists() and link.read_text(encoding="utf-8") == "hello\n",
            "a hardlink must be rewritten through the strip and extract with its target's content",
        )
    ]


# A symlink's target is relative to the member's own directory, which the strip
# moves with it, so it is left alone.
def symlink(destination):
    link = destination / "latest"
    return [
        (
            link.is_symlink() and os.readlink(link) == "greeting.txt",
            "a symlink must survive the strip with its target intact; got "
            f"{os.readlink(link) if link.is_symlink() else 'not a symlink'}",
        )
    ]


fetch_archive(
    "single root",
    [
        ("widget-1.0", "dir", None),
        ("widget-1.0/greeting.txt", "file", "hello\n"),
        ("widget-1.0/deep/nested.txt", "file", "nested\n"),
    ],
    expect_tree=single_root,
)
fetch_archive(
    "hardlink",
    [
        ("widget-1.0", "dir", None),
        ("widget-1.0/greeting.txt", "file", "hello\n"),
        ("widget-1.0/greeting.link", "link", "widget-1.0/greeting.txt"),
    ],
    expect_tree=hardlink,
)
fetch_archive(
    "symlink",
    [
        ("widget-1.0", "dir", None),
        ("widget-1.0/greeting.txt", "file", "hello\n"),
        ("widget-1.0/latest", "symlink", "greeting.txt"),
    ],
    expect_tree=symlink,
)

# An absolute member name's first part is the anchor "/", not a directory: it
# passed the one-component count and the `.`/`..` test, and the strip then
# published the tree *below* the root as the component.
fetch_archive(
    "absolute root",
    [("/etc/pwned", "file", "pwned\n")],
    expect=1,
    needles=("cannot strip it",),
)
# A flat tarball's only top-level component is a file, so there is nothing to
# drop; it must be refused rather than extracted empty.
fetch_archive(
    "flat archive",
    [("greeting.txt", "file", "hello\n")],
    expect=1,
    needles=("is the whole archive",),
)
fetch_archive(
    "two roots",
    [("one/a.txt", "file", "a\n"), ("two/b.txt", "file", "b\n")],
    expect=1,
    needles=("2 top-level components",),
)
fetch_archive(
    "escaping root",
    [("../escape", "file", "x\n")],
    expect=1,
    needles=("cannot strip it",),
)
fetch_archive(
    "escaping member",
    [("widget-1.0", "dir", None), ("widget-1.0/../escape", "file", "x\n")],
    expect=1,
    needles=("escapes the extraction root",),
)
print("fetch-pinned's archive strip accepts the source tree and refuses the rest")

# 5. This repository's own tree is valid, and every group is declared where
# versions.json says it is.
run(root, "check")
# Every real component resolves for every platform to exactly series.common
# followed by series.<platform>, in order, comments and blank lines dropped.
# Running `resolve` at the default expect=0 only proves it did not crash:
# cmd_resolve rejects an unknown platform and nothing else -- a missing series
# file reads as empty -- so an empty or truncated resolution used to pass here.
for component in sorted(p.name for p in (root / "patches").iterdir() if p.is_dir()):
    component_dir = root / "patches" / component
    for platform in ("apple", "android", "linux", "windows"):
        expected = []
        for series_name in ("series.common", f"series.{platform}"):
            series_file = component_dir / series_name
            if not series_file.is_file():
                continue
            expected += [
                line.strip()
                for line in series_file.read_text(encoding="utf-8").splitlines()
                if line.strip() and not line.strip().startswith("#")
            ]
        result = run(root, "resolve", component, platform)
        resolved = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        check(
            resolved == expected,
            f"resolve {component} {platform} must return series.common then "
            f"series.{platform}; got {resolved}, expected {expected}",
        )

# A component a group's build consumes must declare that group in versions.json;
# check enforces it, so prove the enforcement fires on a real tree.
with tempfile.TemporaryDirectory() as tmp:
    sandbox = Path(tmp)
    shutil.copytree(root / "patches", sandbox / "patches")
    # `check` reads one file outside patches/ and platforms/: the apple group
    # has no group.json, so its build list -- every library it compiles -- is
    # read out of the Swift driver. Without it the apple group appears to build
    # nothing and every apple component goes unchecked.
    driver = Path("Sources/BuildScripts/XCFrameworkBuild/main.swift")
    (sandbox / driver).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / driver, sandbox / driver)
    # `check` reads platforms/*/group.json and nothing else under platforms/.
    # The built subtrees are not inputs, they are gigabytes, and
    # platforms/android/prefix/<abi>/usr is a self-symlink, so copying them
    # fails outright on a checkout that has ever been built ("Too many levels
    # of symbolic links"). symlinks=True keeps any other build link from being
    # followed as well.
    built = ("deps", "prefix", "sdk")
    shutil.copytree(
        root / "platforms",
        sandbox / "platforms",
        symlinks=True,
        ignore=shutil.ignore_patterns(*built),
    )
    # The sandbox is only as good as its inputs. Two independent checks, so a
    # future `check` input added under platforms/ cannot silently vanish into
    # the copy:
    #
    # 1. The group.json set matches exactly, and is not empty: an empty pair of
    #    sets would satisfy equality while proving nothing.
    copied_groups = sorted(path.parent.name for path in (sandbox / "platforms").glob("*/group.json"))
    source_groups = sorted(path.parent.name for path in (root / "platforms").glob("*/group.json"))
    check(
        copied_groups and copied_groups == source_groups,
        f"the sandbox must mirror every platforms/*/group.json; copied {copied_groups}, "
        f"source has {source_groups}",
    )
    # 2. Every file git tracks under platforms/ survives the copy. Ignoring a
    #    name is only sound while that name is build output: a tracked
    #    platforms/<group>/deps (or /sdk, /prefix) would be dropped here and
    #    silently validated as absent, which is exactly the divergence this
    #    guards. Untracked build output is invisible to git and stays dropped.
    #    A tree without git (an extracted archive) can still answer 1, so this
    #    one is skipped there rather than failing the run.
    tracked = subprocess.run(
        ["git", "ls-files", "platforms"], cwd=root, capture_output=True, text=True
    )
    if tracked.returncode == 0:
        missing = [name for name in tracked.stdout.split() if not (sandbox / name).exists()]
        check(
            not missing,
            f"the sandbox dropped tracked platforms/ files {missing}; an ignore pattern "
            f"({', '.join(built)}) must only cover build output",
        )
    else:
        print("no git checkout to list tracked platforms/ files from; skipped")

    shutil.copy2(root / "versions.json", sandbox / "versions.json")
    versions = json.loads((sandbox / "versions.json").read_text(encoding="utf-8"))
    versions["components"]["ffmpeg"]["platforms"] = ["apple"]
    (sandbox / "versions.json").write_text(json.dumps(versions), encoding="utf-8")
    result = run(sandbox, "check", expect=1)
    check(
        "ffmpeg" in result.stderr and "android" in result.stderr,
        "check must fail when a built component does not declare its group",
    )

    # The apple set is the driver's build list, so a component only the driver
    # names -- not a series file, not the manifest, which holds three libraries
    # -- is held to its declared platforms too. Deriving the set from
    # series.apple instead left all but ffmpeg and mpv unchecked.
    versions["components"]["ffmpeg"]["platforms"] = ["apple", "android"]
    versions["components"]["libplacebo"]["platforms"] = ["android"]
    (sandbox / "versions.json").write_text(json.dumps(versions), encoding="utf-8")
    result = run(sandbox, "check", expect=1)
    check(
        "apple builds 'libplacebo'" in result.stderr,
        "check must hold every component the apple driver builds to its declared "
        f"platforms; got: {result.stderr.strip()}",
    )

    # And the inputs it reads are required, not optional: a tree that builds
    # components without pinning them has to say so instead of checking
    # nothing. The synthetic fixtures above stay valid -- they carry neither
    # platform groups nor a driver, so there is nothing to declare.
    (sandbox / "versions.json").unlink()
    result = run(sandbox, "check", expect=1)
    check(
        "versions.json" in result.stderr and "missing" in result.stderr,
        f"check must report a missing versions.json, not skip the declaration check; "
        f"got: {result.stderr.strip()}",
    )

    # A driver whose build list cannot be read is the same hole in a different
    # shape: a reformatted `let builds:` line would empty the apple set and
    # retire the check silently, so that has to be a problem of its own.
    driver_copy = (sandbox / driver).read_text(encoding="utf-8")
    (sandbox / driver).write_text("// reformatted: no build list here\n", encoding="utf-8")
    result = run(sandbox, "check", expect=1)
    check(
        "builds: [BaseBuild]" in result.stderr,
        f"check must refuse a driver whose build list it cannot read; got: {result.stderr.strip()}",
    )
    (sandbox / driver).write_text(driver_copy, encoding="utf-8")
print("the repository's own patch tree passes check")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)
print("\nall checks passed")
PY
