#!/usr/bin/env bash
# Regression test for the bounded PCM lookahead in
# patch/libmpv/0015-avfoundation-bound-pcm-lookahead.patch.
#
# The behaviour itself is timing and lifetime, which a stubbed unit test cannot
# reach: it was validated by driving a real AVSampleBufferAudioRenderer, which
# needs an audio device and the patched mpv, so neither belongs in CI. What is
# checkable here are the four structural properties the design depends on, each
# of which was a live bug at some point while writing it.
#
# Checks work per hunk rather than per function: the diff carries only a few
# lines of context, so a whole function is rarely present in one piece. Each
# hunk header names the function it falls in, which is the reliable anchor.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0015-avfoundation-bound-pcm-lookahead.patch"

python3 - "$patch_file" <<'PY'
import re, sys

hunks = []  # (enclosing function, post-image text, added-only text)
current = None
for line in open(sys.argv[1], encoding="utf-8").read().splitlines():
    m = re.match(r"@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@ ?(.*)", line)
    if m:
        current = {"func": m.group(1), "post": [], "added": []}
        hunks.append(current)
        continue
    if current is None or line.startswith(("+++", "---", "diff ", "index ")):
        continue
    if line.startswith("+"):
        current["post"].append(line[1:])
        current["added"].append(line[1:])
    elif line.startswith(" ") or not line:
        current["post"].append(line[1:] if line else "")

def hunk_func(header):
    # "static void uninit(struct ao *ao)" -> "uninit". Exact names matter:
    # a substring test would let uninit satisfy a check meant for init.
    m = re.search(r"(\w+)\s*\(", header)
    return m.group(1) if m else ""

def added_in(func_name):
    return "\n".join("\n".join(h["added"])
                     for h in hunks if hunk_func(h["func"]) == func_name)

failures = []

# 1. The bound must stay off the compressed path. The compressed sink never
#    arms feed() -- start() wires requestMediaDataWhenReadyOnQueue only for
#    PCM, and the AVPlayer path is driven by feed_avp() -- so the gate must
#    sit inside feed(), and the compressed driver must not consult the bound.
gate_found = False
for h in hunks:
    post = "\n".join(h["post"])
    start = post.find("static void feed(struct ao *ao)\n{")
    if start >= 0 and "pcm_lookahead_ns" in post[start:]:
        gate_found = True
if not gate_found:
    failures.append("could not find the lookahead gate inside feed(); update "
                    "this test if feed() was renamed or moved")
for h in hunks:
    added = "\n".join(h["added"])
    if "pcm_lookahead_ns" in added and ("feed_avp" in added or "avp_pull" in added):
        failures.append("the compressed driver touches the lookahead bound; it "
                        "must stay on the PCM path only")

# 2. The timer handler dereferences the AO, which ao_uninit() frees as soon as
#    uninit() returns, so teardown has to cancel it rather than leave a delayed
#    firing to land in freed memory. init()'s error path owns the same duty.
for func, what in (("uninit", "uninit()"), ("init", "init()'s error path")):
    if "dispatch_source_cancel" not in added_in(func):
        failures.append(f"{what} does not cancel feed_timer; a pending fire would "
                        "dereference the freed AO")

# 3. --audio-buffer is documented as a minimum the device may exceed, so it may
#    only raise the bound, never define it.
allsrc = "\n".join("\n".join(h["post"]) for h in hunks)
if not re.search(r"MPMAX\(\s*p->opt_max_lookahead,\s*ao->def_buffer\s*\)", allsrc):
    failures.append("def_buffer is no longer a floor under the bound; it is "
                    "documented as a minimum and cannot be used as the maximum")

# 4. A frozen clock never drains the queue, so an ungated handler re-arms every
#    half-bound forever while paused.
handler = re.search(r"dispatch_source_set_event_handler\(p->feed_timer, \^\{.*?\}\);",
                    allsrc, re.S)
if not handler:
    failures.append("could not find the timer handler; update this test if it moved")
elif "feed_enabled" not in handler.group(0):
    failures.append("the timer handler does not check feed_enabled; a delivery "
                    "already in flight would re-arm feeding after pause or stop")

# 5. The bound is macOS-only. tvOS and iOS keep the renderer's own deep queue,
#    which is what carries AirPlay, and their hardware volume never reaches
#    mpv's software gain in the first place. Every line that touches the
#    feature must sit inside a TARGET_OS_OSX region, so those platforms
#    preprocess to exactly the source they had before.
sentinels = ("pcm_lookahead_ns", "feed_timer", "feed_enabled", "opt_max_lookahead",
             "pcm_set_feeding", "pcm_pause_feeding", "pcm_request_media_data",
             "pcm_cancel_pending_feed")
for h in hunks:
    depth, osx = 0, None  # osx = depth at which a TARGET_OS_OSX gate opened
    for line in h["post"]:
        t = line.strip()
        if t.startswith("#if"):
            depth += 1
            if osx is None and "TARGET_OS_OSX" in t:
                osx = depth
        elif t.startswith("#endif"):
            if osx == depth:
                osx = None
            depth -= 1
        elif osx is None and any(s in t for s in sentinels) and not t.startswith("//"):
            failures.append(f"'{t[:56]}' in {hunk_func(h['func'])}() is outside a "
                            "TARGET_OS_OSX gate; the bound must not exist on "
                            "tvOS or iOS")
            break

for f in failures:
    print(f"FAIL: {f}", file=sys.stderr)
sys.exit(1 if failures else 0)
PY

echo "all 5 PCM lookahead invariants hold"
