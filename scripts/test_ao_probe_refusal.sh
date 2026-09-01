#!/usr/bin/env bash
# Structural regression test for
# patch/libmpv/0023-avfoundation-refuse-reads-past-the-write-head.patch.
#
# Real compressed playback needs an Apple audio route and cannot run in CI.
# These checks defend the loader contract: a read past the write head is
# refused instead of answered with fabricated access units. CoreMedia caches
# whatever the loader serves by stream offset and plans its sequential reads
# around the cached range, so fabricated probe bytes at the parser's fixed
# ~183 MiB probe offset become the audio played once the stream reaches that
# byte (~30-40 min at DD+ rates), and the follow-up read lands past the write
# head where it used to be fed fabricated frames forever (plezy#1776).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$root/patches/mpv/pool/0023-avfoundation-refuse-reads-past-the-write-head.patch"

python3 - "$patch_file" <<'PY'
import sys

lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
added = "\n".join(line[1:] for line in lines
                  if line.startswith("+") and not line.startswith("+++"))
removed = "\n".join(line[1:] for line in lines
                    if line.startswith("-") and not line.startswith("---"))

failures = []

# 1. The fabricated-probe reply is gone: no synthesized bytes, no retained
# last access unit to synthesize from.
for needle in ("synthesized %lu probe bytes", "au_buf", "au_len"):
    if needle in added:
        failures.append(f"fix reintroduces {needle!r}")
    if needle not in removed:
        failures.append(f"fix does not remove {needle!r}")

# 2. Reads past the write head are refused with an error so the system falls
# back to plain sequential reads; leaving them pending would park preroll.
if "off > write_head" not in added:
    failures.append("missing the beyond-write-head branch")
if "finishLoadingWithError" not in added or "ENOTSUP" not in added:
    failures.append("beyond-write-head reads are not refused with an error")

# 3. The pinned sequential read (off == write_head) must stay pending, not
# refused: it is served on the next append. The refusal must be strict.
if "off >= write_head" in added:
    failures.append("refusal must not swallow the pinned write-head read")

if failures:
    for failure in failures:
        print(f"FAIL: {failure}")
    sys.exit(1)
print("ok: reads past the write head are refused, nothing is fabricated")
PY
