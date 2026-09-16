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
import json
import os
import shutil
import subprocess
import sys
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

# 5. This repository's own tree is valid, and every group is declared where
# versions.json says it is.
run(root, "check")
# Every real component resolves for every platform, and every entry it names
# exists: `check` above is what enforces the latter, this proves the former.
for component in sorted(p.name for p in (root / "patches").iterdir() if p.is_dir()):
    for platform in ("apple", "android", "linux", "windows"):
        run(root, "resolve", component, platform)

# A component a group's build consumes must declare that group in versions.json;
# check enforces it, so prove the enforcement fires on a real tree.
with tempfile.TemporaryDirectory() as tmp:
    sandbox = Path(tmp)
    shutil.copytree(root / "patches", sandbox / "patches")
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
print("the repository's own patch tree passes check")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)
print("\nall checks passed")
PY
