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
import os
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

# 2. resolve --hashes: `name<TAB>sha256hex` of the pool bytes.
with tempfile.TemporaryDirectory() as tmp:
    component = make_tree(tmp)
    result = run(tmp, "resolve", "widget", "apple", "--hashes")
    lines = result.stdout.splitlines()
    check(len(lines) == 3, f"--hashes must emit one line per resolved patch; got {len(lines)}")
    for line in lines:
        name, _, digest = line.partition("\t")
        expected = hashlib.sha256((component / "pool" / name).read_bytes()).hexdigest()
        check(digest == expected, f"--hashes digest for {name} must be sha256 of the pool bytes")
    print("--hashes emits name<TAB>sha256hex")

# 3. check: pass on a clean tree, fail on each contract violation.
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

# 5. This repository's own tree is valid, and the apple series are nonempty.
# series.common resolves first; behind it, the migrated apple series keeps
# the old lexicographic apply order and its patch count.
def series_entries(path):
    if not path.is_file():
        return []
    lines = (line.strip() for line in path.read_text(encoding="utf-8").splitlines())
    return [line for line in lines if line and not line.startswith("#")]


result = run(root, "check")
for component, migrated_count in (("mpv", 24), ("ffmpeg", 13)):
    common = series_entries(root / "patches" / component / "series.common")
    resolved = run(root, "resolve", component, "apple").stdout.split()
    check(resolved[: len(common)] == common, f"the real {component} apple series must start with series.common")
    migrated = resolved[len(common):]
    check(
        len(migrated) == migrated_count,
        f"the real {component} apple series must resolve {migrated_count} apple patches, got {len(migrated)}",
    )
    check(migrated == sorted(migrated), f"the migrated {component} series must preserve the old lexicographic order")
print("the repository's own patch tree passes check")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)
print("\nall checks passed")
PY
