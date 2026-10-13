#!/usr/bin/env bash
# Regression test for the android release tarballs: no file in them may carry
# the clock of the build that made it.
#
# publish-assets refuses a rebuilt content key whose bytes differ from the
# asset already published under its name, and a key is rebuilt whenever a run
# uploads its assets but loses its manifest push. A library that stamps its
# build time therefore publishes fine once and fails every rebuild: libmpv.so's
# __DATE__/__TIME__ failed run 36130739211 (key 261aab02d278, fixed by
# -Dbuild-date=false), and OpenSSL's "built on: <date> UTC", which ffmpeg
# links into libavformat.so, failed run 38403868661 (key e86108ac4eb2). Both
# shipped and passed every build leg; this check fails the leg instead.
#
# Usage: test_android_build_date.sh [--day YYYY-MM-DD]... [tarball...]
#
# Every run first proves the scan on synthetic tarballs (one stamped in each
# clock format the scan knows, one stamped with the driver's fixed epoch).
# Then each tarball given is unpacked and every file searched for a clock
# stamp of the build days: today and yesterday, in UTC and in local time (a
# leg can cross midnight), unless --day names them. --day is how an old
# tarball is checked against the day it was built.
set -euo pipefail

python3 - "$@" <<'PY'
import datetime as dt
import io
import re
import sys
import tarfile
import tempfile
from pathlib import Path

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


args = sys.argv[1:]
days = []
tarballs = []
while args:
    arg = args.pop(0)
    if arg == "--day":
        if not args:
            sys.exit("usage: test_android_build_date.sh [--day YYYY-MM-DD]... [tarball...]")
        days.append(dt.date.fromisoformat(args.pop(0)))
    else:
        tarballs.append(arg)
if not days:
    now = dt.datetime.now(dt.timezone.utc)
    for today in (now.date(), now.astimezone().date()):
        days += [today, today - dt.timedelta(days=1)]
days = sorted(set(days))

MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()
WEEKDAYS = "Mon Tue Wed Thu Fri Sat Sun".split()


def stamps(day):
    """Byte patterns of a clock reading taken on `day`.

    Only spellings that come from a clock: a bare date is how sources spell
    release dates (OpenSSL's "1 Oct 2026"), so each pattern is either
    __DATE__'s own spelling or carries a time of day.
    """
    b, a, y = MONTHS[day.month - 1], WEEKDAYS[day.weekday()], day.year
    e = f"{day.day:2d}"  # asctime and __DATE__ pad the day with a space
    d = f"{day.day:02d}"
    hms = r"\d\d:\d\d:\d\d"
    return [
        # __DATE__ ("Oct 13 2026"), what mpv stamped before -Dbuild-date=false
        ("__DATE__", rf"{b} {e} {y}"),
        # asctime/ctime, Perl's scalar gmtime: OpenSSL's mkbuildinf.pl
        ("asctime", rf"{a} {b} {e} {hms} (?:[A-Z]{{3,5}} )?{y}"),
        # ISO 8601 with a time of day
        ("ISO 8601", rf"{y}-{day.month:02d}-{d}[T ]\d\d:\d\d"),
        # RFC 2822 (date -R)
        ("RFC 2822", rf"{a}, {d} {b} {y} {hms}"),
    ]


patterns = [
    (day, name, re.compile(pattern.encode())) for day in days for name, pattern in stamps(day)
]


def scan(path):
    """(member, offset, format, text) for every clock stamp in a tarball."""
    found = []
    with tarfile.open(path) as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            data = archive.extractfile(member).read()
            for day, name, pattern in patterns:
                for match in pattern.finditer(data):
                    # The printable run around the stamp, for the report.
                    start = match.start()
                    while start > 0 and 0x20 <= data[start - 1] < 0x7F and match.start() - start < 60:
                        start -= 1
                    end = match.end()
                    while end < len(data) and 0x20 <= data[end] < 0x7F and end - match.end() < 60:
                        end += 1
                    found.append((member.name, match.start(), f"{name}, {day}", data[start:end].decode("ascii")))
    return found


def fixture(path, payload):
    with tarfile.open(path, "w:gz") as archive:
        info = tarfile.TarInfo("lib/libprobe.so")
        info.size = len(payload)
        archive.addfile(info, io.BytesIO(payload))


# Self-test: the scan has to see each format it claims, and nothing in a
# library built with the driver's fixed epoch (SOURCE_DATE_EPOCH=1577836800).
with tempfile.TemporaryDirectory() as tmp:
    day = days[-1]
    clock = dt.datetime.combine(day, dt.time(12, 39, 4))
    b, a = MONTHS[day.month - 1], WEEKDAYS[day.weekday()]
    spellings = {
        "__DATE__": f"{b} {day.day:2d} {day.year}\0{clock:%H:%M:%S}",
        "asctime": f"built on: {a} {b} {day.day:2d} {clock:%H:%M:%S} {day.year} UTC",
        "ISO 8601": f"{clock:%Y-%m-%dT%H:%M:%SZ}",
        "RFC 2822": f"{a}, {day.day:02d} {b} {day.year} {clock:%H:%M:%S} +0000",
    }
    for name, text in spellings.items():
        path = Path(tmp) / f"{name}.tar.gz"
        fixture(path, b"\x7fELF\0\0" + text.encode() + b"\0\x01\x02")
        formats = {hit[2].split(",")[0] for hit in scan(path)}
        check(name in formats, f"self-test: a {name} stamp {text!r} is not detected ({sorted(formats)})")
    path = Path(tmp) / "fixed.tar.gz"
    fixture(path, b"\x7fELF\0\0built on: Wed Jan  1 00:00:00 2020 UTC\0OpenSSL 4.0.3 1 Oct 2026\0")
    check(not scan(path), "self-test: the fixed-epoch stamp and a release date must not be detected")
if failures:
    sys.exit(f"\n{len(failures)} failure(s): the scan itself is broken")
print(f"self-test: every clock format detected, the fixed epoch is clean ({', '.join(map(str, days))})")

for path in tarballs:
    found = scan(path)
    for member, offset, how, text in found:
        check(False, f"{path}: {member} @0x{offset:x} carries the build clock ({how}): {text!r}")
    if not found:
        print(f"{path}: no build clock in any file")

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
PY
