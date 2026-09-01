#!/usr/bin/env bash
# Structural regression test for
# patch/libmpv/0019-avfoundation-configurable-compressed-audio.patch.
#
# Real compressed playback needs an Apple audio route and cannot run in CI.
# These checks defend the option contract: current behavior stays the default,
# disabling it rejects only SPDIF before AVFoundation owns any resources, and
# every Apple target can select the behavior.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0019-avfoundation-configurable-compressed-audio.patch"

python3 - "$patch_file" <<'PY'
import re
import sys

lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
hunks = []
current = None
for line in lines:
    match = re.match(r"@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@ ?(.*)", line)
    if match:
        current = {"header": match.group(1), "post": [], "added": []}
        hunks.append(current)
        continue
    if current is None or line.startswith(("+++", "---", "diff ", "index ")):
        continue
    if line.startswith("+"):
        current["post"].append(line[1:])
        current["added"].append(line[1:])
    elif line.startswith(" ") or not line:
        current["post"].append(line[1:] if line else "")

failures = []
added = "\n".join(line for hunk in hunks for line in hunk["added"])

# 1. The public option is stable and defaults to the behavior existing users
# already receive. A dependency bump must not silently disable Apple Dolby
# playback on tvOS or iOS.
for expected in (
    "bool opt_accept_compressed;",
    ".opt_accept_compressed = true,",
    '{"accept-compressed", OPT_BOOL(opt_accept_compressed)},',
):
    if expected not in added:
        failures.append(f"missing option contract line: {expected}")

# 2. Opting out rejects only encoded audio. PCM must continue through the
# AVSampleBufferAudioRenderer path exactly as before.
init_hunk = next((hunk for hunk in hunks if "static int init(struct ao *ao)" in hunk["header"]), None)
if init_hunk is None:
    failures.append("could not find init() hunk")
else:
    init_post = "\n".join(init_hunk["post"])
    gate = re.compile(
        r"bool spdif = af_fmt_is_spdif\(ao->format\);\s+"
        r"if \(spdif && !p->opt_accept_compressed\) \{\s+"
        r'MP_VERBOSE\(ao, "compressed audio disabled; trying next audio output\\n"\);\s+'
        r"return CONTROL_ERROR;\s+\}",
        re.S,
    )
    if not gate.search(init_post):
        failures.append("init() no longer rejects only SPDIF immediately after format detection")

# 3. The rejection must happen before AVAudioSession configuration, queue
# allocation, or AVPlayer construction. Keeping the gate directly after the
# format test makes the opt-out ownership-free and lets mpv try the next AO.
if init_hunk is not None:
    init_post = "\n".join(init_hunk["post"])
    gate_end = init_post.find("return CONTROL_ERROR;")
    first_following_state = init_post.find("if (!spdif)")
    if gate_end < 0 or first_following_state < 0 or gate_end > first_following_state:
        failures.append("compressed opt-out no longer returns before AO state changes")

# 4. This is an AO capability switch, not a macOS-only workaround. The field
# and option must remain outside platform preprocessor guards so a host can A/B
# the compressed sink on every Apple target.
def preprocessor_depth_at(hunk, needle):
    depth = 0
    for line in hunk["post"]:
        stripped = line.strip()
        if stripped.startswith("#if"):
            depth += 1
        elif stripped.startswith("#endif"):
            depth -= 1
        if needle in line:
            return depth
    return None

for needle in ("bool opt_accept_compressed;", '"accept-compressed"'):
    owner = next((hunk for hunk in hunks if any(needle in line for line in hunk["post"])), None)
    depth = preprocessor_depth_at(owner, needle) if owner else None
    if depth != 0:
        failures.append(f"{needle} is missing or platform-gated")

for failure in failures:
    print(f"FAIL: {failure}", file=sys.stderr)
sys.exit(1 if failures else 0)
PY

echo "all 4 compressed-audio option invariants hold"
