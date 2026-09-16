#!/usr/bin/env bash
# Structural regression test for the Apple compressed-audio loader contract: a
# read past the write head is refused instead of answered with fabricated
# access units.
#
# Real compressed playback needs an Apple audio route and cannot run in CI, so
# these checks read the shipped loader out of the applied series. CoreMedia
# caches whatever the loader serves by stream offset and plans its sequential
# reads around the cached range, so fabricated probe bytes at the parser's fixed
# ~183 MiB probe offset become the audio played once the stream reaches that
# byte (~30-40 min at DD+ rates), and the follow-up read lands past the write
# head where it used to be fed fabricated frames forever (plezy#1776).
#
# The assertions run against the tree the series produces, not against one
# patch's diff: which patch introduces or removes the machinery is an
# implementation detail the contract does not care about. Pass an already
# patched mpv tree for offline use; otherwise fetch and patch the pinned source.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${1:-}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

if [[ -z "$source_dir" ]]; then
    source_dir="$workdir/source"
    # The pin contract lives in scripts/patches.py: it resolves the
    # override-aware pin, clones the ref, proves HEAD is the pinned commit
    # and applies the resolved mpv/apple series.
    (cd "$root" && python3 scripts/patches.py fetch-pinned mpv apple "$source_dir")
fi

python3 - "$source_dir/audio/out/ao_avfoundation.m" <<'PY'
import re
import sys
from pathlib import Path

source = Path(sys.argv[1])
if not source.is_file():
    raise SystemExit(f"FAIL: {source} is missing from the applied tree")
text = source.read_text(encoding="utf-8")
failures = []

# 1. Nothing is fabricated: no synthesized probe bytes and no retained last
# access unit to synthesize them from.
for needle in ("synthesized %lu probe bytes", "au_buf", "au_len"):
    if needle in text:
        failures.append(f"the shipped loader still carries {needle!r}")

# 2. Reads past the write head are refused with an error so the system falls
# back to plain sequential reads; leaving them pending would park preroll.
if "off > write_head" not in text:
    failures.append("missing the beyond-write-head branch")
if "finishLoadingWithError" not in text or "ENOTSUP" not in text:
    failures.append("beyond-write-head reads are not refused with an error")

# 3. The pinned sequential read (off == write_head) must stay pending, not
# refused: it is served on the next append. The refusal must be strict.
if re.search(r"off\s*>=\s*write_head", text):
    failures.append("refusal must not swallow the pinned write-head read")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("ok: reads past the write head are refused, nothing is fabricated")
PY
