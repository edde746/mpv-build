#!/usr/bin/env bash
# Regression test for scripts/extract.py, the shared extractor the host
# harnesses slice production code with. Four operations, each with a rule the
# harnesses depend on and none of which they can check for themselves: a
# harness compiles what it is handed, so an extractor that hands it a slightly
# different chunk than it claims is invisible from inside the harness.
#
# The fixture cases are synthetic so the assertions can be exact (which chunk,
# which failure) without touching the network or a pinned source tree. One
# sweep runs over this repository's own patch pool, where the assertion is a
# property of every hunk that no fixture can cover as well.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$root" <<'PY'
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
tool = root / "scripts" / "extract.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(*args, expect=0):
    result = subprocess.run(
        [sys.executable, str(tool), *args], capture_output=True, text=True, cwd=root
    )
    if expect is not None and result.returncode != expect:
        failures.append(f"{' '.join(args)} exited {result.returncode}, expected {expect}")
        print(f"FAIL: {' '.join(args)} exited {result.returncode}, expected {expect}")
        print(result.stdout, result.stderr)
    return result


def fixture(directory, name, text):
    path = Path(directory) / name
    path.write_text(text, encoding="utf-8")
    return path


# 1. A hunk ends where its header says it does. `git format-patch` puts a blank
# line before its `-- \n<version>` signature, and the last hunk of the patch sat
# right above it: absorbing it hands the harnesses a post-image one line longer
# than the patch wrote, and the post-image is what they compile.
TRAILER = """\
diff --git a/greet.c b/greet.c
index 1111111..2222222 100644
--- a/greet.c
+++ b/greet.c
@@ -1,4 +1,5 @@ static int greet(void)
 int greet(void)
 {
+    return 1;
     return 0;
 }
 
-- 
2.43.0
"""

# A hunk whose last line carries no trailing newline, spelled the way git
# spells it: the marker annotates the line above and is not one of its lines.
NO_NEWLINE = """\
diff --git a/greet.c b/greet.c
index 1111111..2222222 100644
--- a/greet.c
+++ b/greet.c
@@ -1 +1 @@
-old
+new
\\ No newline at end of file
"""

with tempfile.TemporaryDirectory() as tmp:
    hunks_out = Path(tmp) / "hunks.json"
    run("hunks", str(fixture(tmp, "trailer.patch", TRAILER)), str(hunks_out))
    hunks = json.loads(hunks_out.read_text(encoding="utf-8"))
    check(len(hunks) == 1, f"the trailer fixture must parse as one hunk, got {len(hunks)}")
    check(
        hunks[0]["post"] == ["int greet(void)", "{", "    return 1;", "    return 0;", "}"],
        f"a hunk must stop at its header's count, got {hunks[0]['post']!r}",
    )

    run("hunks", str(fixture(tmp, "no-newline.patch", NO_NEWLINE)), str(hunks_out))
    hunks = json.loads(hunks_out.read_text(encoding="utf-8"))
    check(
        hunks[0]["post"] == ["new"],
        f"the no-newline marker is not a hunk line, got {hunks[0]['post']!r}",
    )
    print("hunks: a hunk stops at its header's counts")

# 2. A patch that is not a patch: a pure deletion has an empty post-image, and
# writing that out as an empty file would report success for a chunk a harness
# goes on to compile.
with tempfile.TemporaryDirectory() as tmp:
    result = run(
        "post-image",
        str(fixture(tmp, "deletion.patch", "--- a/greet.c\n+++ b/greet.c\n@@ -1,2 +0,0 @@\n-a\n-b\n")),
        str(Path(tmp) / "out.c"),
        expect=1,
    )
    check(
        "empty" in result.stderr,
        f"an empty post-image must be named as such, got {result.stderr.strip()!r}",
    )
    print("post-image: an empty post-image is a failure, not an empty file")

# 3. `symbol` extracts definitions, in a file ffmpeg compiles one arm at a time.
with tempfile.TemporaryDirectory() as tmp:
    arms = fixture(
        tmp,
        "arms.c",
        "#if CONFIG_NDK\n"
        "static int helper(void)\n{\n    return 1;\n}\n"
        "#else\n"
        "static int helper(void)\n{\n    return 2;\n}\n"
        "#endif\n",
    )
    out = Path(tmp) / "arms.inc"
    run("symbol", str(arms), str(out), "--fn", "helper", "--return", "int")
    check(
        out.is_file() and "return 1;" in out.read_text(encoding="utf-8"),
        "the definition of the arm ffmpeg compiles must be the one extracted",
    )

    twice = fixture(
        tmp,
        "twice.c",
        "static int helper(void)\n{\n    return 1;\n}\n\n"
        "static int helper(void)\n{\n    return 2;\n}\n",
    )
    result = run("symbol", str(twice), str(Path(tmp) / "twice.inc"), "--fn", "helper", expect=1)
    check(
        "more than once" in result.stderr,
        f"two definitions that compile together must be refused, got {result.stderr.strip()!r}",
    )

    declared = fixture(tmp, "declared.c", "static int helper(void);\n")
    result = run("symbol", str(declared), str(Path(tmp) / "declared.inc"), "--fn", "helper", expect=1)
    check(
        "no definition" in result.stderr,
        f"a declaration without a body is not a definition, got {result.stderr.strip()!r}",
    )

    # A pointer return type ends in the `*` that touches the name.
    pointer = fixture(tmp, "pointer.c", 'const char *source(void)\n{\n    return "x";\n}\n')
    out = Path(tmp) / "pointer.inc"
    run("symbol", str(pointer), str(out), "--fn", "source", "--return", "const char *")
    check(
        out.is_file() and out.read_text(encoding="utf-8").startswith("const char *source(void)"),
        "an explicit pointer return type must match a definition that has no space before its name",
    )
    print("symbol: definitions only, one arm of a conditional, literal return types")

# 4. A continued `#define` is one chunk, and a continuation that runs off the
# end of the file is a truncated source rather than a chunk to emit.
with tempfile.TemporaryDirectory() as tmp:
    truncated = fixture(tmp, "truncated.c", "#define LOOP(x) \\\n")
    result = run("define", str(truncated), str(Path(tmp) / "truncated.inc"), "--name", "LOOP", expect=1)
    check(
        "never ends" in result.stderr,
        f"an unterminated continuation must be reported, got {result.stderr.strip()!r}",
    )
    print("define: an unterminated continuation is refused")

# 5. Regional extraction: the marker must be there, and --added-only is only
# meaningful for a patch.
with tempfile.TemporaryDirectory() as tmp:
    marked = fixture(
        tmp,
        "marked.c",
        "// --- core\n\nint core(void)\n{\n    return 0;\n}\n\n// --- end core\n",
    )
    run("region", str(marked), str(Path(tmp) / "core.inc"), "--start", "core")
    result = run(
        "region",
        str(marked),
        str(Path(tmp) / "missing.inc"),
        "--start",
        "core",
        "--require",
        "not present",
        expect=1,
    )
    check(
        "not present" in result.stderr,
        f"a --require the region does not carry must fail, got {result.stderr.strip()!r}",
    )
    result = run(
        "region", str(marked), str(Path(tmp) / "added.inc"), "--start", "core", "--added-only", expect=1
    )
    check(
        "--added-only" in result.stderr,
        f"--added-only without --patch must be refused, got {result.stderr.strip()!r}",
    )
    print("region: markers, --require and --added-only all hold")

# 6. The repository's own pool: every hunk of every pool patch must parse to
# exactly the post-image its header declares. This is the property the harnesses
# assume when they compile a hunk, and the one the format-patch trailer used to
# break for the last hunk of a patch.
sys.path.insert(0, str(root / "scripts"))
import extract  # noqa: E402

HEADER = re.compile(r"@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
pool = sorted((root / "patches").glob("*/pool/*.patch"))
check(len(pool) > 50, f"the pool sweep must see the real patch pool, found {len(pool)} files")
hunks = 0
mismatched = []
for patch in pool:
    declared_counts = [
        int(match.group(3)) if match.group(3) is not None else 1
        for match in (
            HEADER.match(line) for line in patch.read_text(encoding="utf-8").splitlines()
        )
        if match
    ]
    parsed = extract.extract_hunks(patch)
    hunks += len(parsed)
    if len(declared_counts) != len(parsed):
        mismatched.append((patch.name, "hunk count", len(declared_counts), len(parsed)))
        continue
    for count, hunk in zip(declared_counts, parsed):
        if len(hunk["post"]) != count:
            mismatched.append((patch.name, hunk["header"][:48], count, len(hunk["post"])))
check(hunks > 500, f"the pool sweep must parse the real hunks, found {hunks}")
check(
    not mismatched,
    f"every hunk must parse to its header's count; {len(mismatched)} do not, e.g. {mismatched[:3]}",
)
print(f"the repository's own pool: {hunks} hunks, each as long as its header declares")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    raise SystemExit(1)
print("\nall checks passed")
PY
