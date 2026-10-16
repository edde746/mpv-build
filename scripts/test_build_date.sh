#!/usr/bin/env bash
# Regression test for every group's release assets: no file in them may carry
# the clock of the build that made it.
#
# publish-assets refuses a rebuilt content key whose bytes differ from the
# asset already published under its name, and a key is rebuilt whenever a run
# uploads its assets but loses its manifest push. A library that stamps its
# build time therefore publishes fine once and fails every rebuild: libmpv.so's
# __DATE__/__TIME__ failed run 36130739211 (android key 261aab02d278, fixed by
# -Dbuild-date=false), and OpenSSL's "built on: <date> UTC", which ffmpeg
# links into libavformat.so, failed run 38403868661 (android key
# e86108ac4eb2). libmpv-2.dll in windows key 442b5d101e4c carries five: the
# PE header and debug directory stamps lld-link takes from the clock, and
# OpenSSL's "built on", SVT-AV1's and mpv's __DATE__ __TIME__ and davs2's
# build time, which the cached build tree had kept from the runs that
# compiled them. All of them shipped and passed every build leg; this check
# fails the leg instead.
#
# Usage: test_build_date.sh [--day YYYY-MM-DD]... [asset...]
#
# Every run first proves the scan on synthetic assets. Then each asset given
# (.tar.gz, .tar.zst, .zip or .aar, whose classes.jar is searched too) is
# unpacked in memory and searched for:
#   - a clock reading spelled as text: __DATE__ ("Oct 13 2026"), asctime and
#     ctime (OpenSSL's mkbuildinf.pl, __TIMESTAMP__), ISO 8601 with a time of
#     day, RFC 2822 (date -R). Bare dates are left alone, since that is how
#     sources spell release dates.
#   - a clock reading in a binary header: the PE TimeDateStamp and its copies
#     in the debug, export, import, resource, delay-import and load-config
#     directories, the mtime of every member of an ar archive (static and
#     import libraries) and the TimeDateStamp of the COFF objects in it, and
#     the timestamp of a Mach-O dylib load command.
#   - the asset's own entry mtimes (tar headers, zip DOS dates and extended
#     timestamps).
# A reading counts when it falls on a build day. By default that is any day
# from 2020-01-02 (every driver pins SOURCE_DATE_EPOCH and its archive mtimes
# to 2020-01-01, 1577836800) through tomorrow, in UTC or local time: a build
# tree restored from a cache keeps the stamps of whichever run compiled each
# package, weeks before the leg that links it. --day names the build days
# instead, which is how an old asset is checked against the day it was built.
set -euo pipefail

python3 - "$@" <<'PY'
import datetime as dt
import io
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path

USAGE = "usage: test_build_date.sh [--day YYYY-MM-DD]... [asset...]"
EPOCH = 1577836800  # 2020-01-01 00:00:00 UTC, every driver's SOURCE_DATE_EPOCH

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


args = sys.argv[1:]
days = []
assets = []
while args:
    arg = args.pop(0)
    if arg == "--day":
        if not args:
            sys.exit(USAGE)
        days.append(dt.date.fromisoformat(args.pop(0)))
    elif arg.startswith("-"):
        sys.exit(USAGE)
    else:
        assets.append(arg)

now = dt.datetime.now(dt.timezone.utc)
today = max(now.date(), now.astimezone().date())
if days:
    days = sorted(set(days))
    window = ", ".join(map(str, days))

    def build_day(day):
        return day in days
else:
    first = dt.date(2020, 1, 2)
    last = today + dt.timedelta(days=1)
    window = f"{first} to {last}"

    def build_day(day):
        return first <= day <= last


def clock(seconds):
    """The build day a numeric stamp reads as, or None."""
    if seconds <= 0:
        return None
    try:
        moment = dt.datetime.fromtimestamp(seconds, dt.timezone.utc)
    except (OverflowError, OSError, ValueError):
        return None
    for day in (moment.date(), moment.astimezone().date()):
        if build_day(day):
            return day
    return None


MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()
MON = "|".join(MONTHS)
WEEKDAYS = "Mon Tue Wed Thu Fri Sat Sun".split()
WDAY = "|".join(WEEKDAYS)
HMS = r"\d\d:\d\d:\d\d"
TEXT = [
    # __DATE__ ("Oct 13 2026", the day padded with a space)
    ("__DATE__", rf"(?P<b>{MON}) (?P<d>[ 123]\d) (?P<y>\d{{4}})"),
    # asctime/ctime and Perl's scalar gmtime: OpenSSL's mkbuildinf.pl
    ("asctime", rf"(?:{WDAY}) (?P<b>{MON}) (?P<d>[ 123]\d) {HMS} (?:[A-Z]{{3,5}} )?(?P<y>\d{{4}})"),
    # ISO 8601 with a time of day
    ("ISO 8601", r"(?P<y>\d{4})-(?P<m>\d\d)-(?P<d>\d\d)[T ]\d\d:\d\d"),
    # RFC 2822 (date -R)
    ("RFC 2822", rf"(?:{WDAY}), (?P<d>\d\d) (?P<b>{MON}) (?P<y>\d{{4}}) {HMS}"),
]
TEXT = [(name, re.compile(pattern.encode())) for name, pattern in TEXT]


def text_stamps(data):
    """(offset, format, day, text) for every clock reading spelled in data."""
    for name, pattern in TEXT:
        for match in pattern.finditer(data):
            fields = match.groupdict()
            month = int(fields["m"]) if fields.get("m") else MONTHS.index(fields["b"].decode()) + 1
            try:
                day = dt.date(int(fields["y"]), month, int(fields["d"]))
            except ValueError:
                continue
            if not build_day(day):
                continue
            # The printable run around the stamp, for the report.
            start = match.start()
            while start > 0 and 0x20 <= data[start - 1] < 0x7F and match.start() - start < 60:
                start -= 1
            end = match.end()
            while end < len(data) and 0x20 <= data[end] < 0x7F and end - match.end() < 60:
                end += 1
            yield match.start(), name, day, data[start:end].decode("ascii")


def u16(data, at):
    return struct.unpack_from("<H", data, at)[0]


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def pe_fields(data):
    """(field, offset, value) for every timestamp field of a PE image."""
    lfanew = u32(data, 0x3C)
    if data[lfanew:lfanew + 4] != b"PE\0\0":
        return
    coff = lfanew + 4
    sections, optional_size = u16(data, coff + 2), u16(data, coff + 16)
    yield "PE TimeDateStamp", coff + 4, u32(data, coff + 4)
    optional = coff + 20
    count_at = optional + (108 if u16(data, optional) == 0x20B else 92)
    directories = [struct.unpack_from("<II", data, count_at + 4 + 8 * i)
                   for i in range(min(u32(data, count_at), 16))]
    table = optional + optional_size
    spans = []
    for i in range(sections):
        size, rva, raw_size, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        spans.append((rva, max(size, raw_size), raw))

    def at(index):
        if index >= len(directories) or not directories[index][0]:
            return None, 0
        rva, size = directories[index]
        for start, length, raw in spans:
            if start <= rva < start + length:
                return rva - start + raw, size
        return None, 0

    for index, name in ((0, "export"), (2, "resource"), (10, "load config")):
        offset, _ = at(index)
        if offset is not None:
            yield f"{name} directory TimeDateStamp", offset + 4, u32(data, offset + 4)
    offset, _ = at(1)
    while offset is not None and any(struct.unpack_from("<5I", data, offset)):
        yield "import descriptor TimeDateStamp", offset + 4, u32(data, offset + 4)
        offset += 20
    offset, _ = at(13)
    while offset is not None and any(struct.unpack_from("<8I", data, offset)):
        yield "delay import TimeDateStamp", offset + 28, u32(data, offset + 28)
        offset += 32
    offset, size = at(6)
    for entry in range(size // 28 if offset is not None else 0):
        yield "debug directory TimeDateStamp", offset + 28 * entry + 4, u32(data, offset + 28 * entry + 4)


COFF_MACHINES = {0x14C, 0x8664, 0xAA64, 0x1C4, 0xA641, 0xA64E}


def ar_fields(data):
    """(field, offset, value) for every ar member mtime and COFF object stamp."""
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        if header[58:60] != b"`\n":
            return
        name = header[:16].decode("ascii", "replace").strip()
        size = int(header[48:58].decode().strip() or 0)
        yield f"ar member {name!r} mtime", offset + 16, int(header[16:28].decode().strip() or 0)
        body = data[offset + 60:offset + 60 + size]
        if body[:4] == b"\0\0\xff\xff" and len(body) >= 12:
            yield f"ar member {name!r} import object TimeDateStamp", offset + 68, u32(body, 8)
        elif len(body) >= 20 and u16(body, 0) in COFF_MACHINES:
            yield f"ar member {name!r} COFF TimeDateStamp", offset + 64, u32(body, 4)
        offset += 60 + size + (size & 1)


DYLIB_COMMANDS = {0xC, 0xD, 0x20, 0x80000018, 0x8000001F, 0x80000023}


def macho_fields(data, base=0):
    """(field, offset, value) for every dylib load command's timestamp."""
    magic = struct.unpack_from(">I", data, base)[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF) and base == 0:
        count = struct.unpack_from(">I", data, 4)[0]
        if count > 16:  # a Java class file shares the magic
            return
        width = 20 if magic == 0xCAFEBABE else 32
        for i in range(count):
            start = struct.unpack_from(">Q" if width == 32 else ">I", data, 8 + width * i + 8)[0]
            yield from macho_fields(data, start)
        return
    if magic not in (0xCEFAEDFE, 0xCFFAEDFE):
        return
    commands = u32(data, base + 16)
    offset = base + (32 if magic == 0xCFFAEDFE else 28)
    for _ in range(commands):
        command, size = u32(data, offset), u32(data, offset + 4)
        if command in DYLIB_COMMANDS:
            yield "Mach-O dylib timestamp", offset + 12, u32(data, offset + 12)
        if size < 8:
            return
        offset += size


def header_fields(data):
    try:
        if data[:2] == b"MZ" and len(data) >= 0x40:
            yield from pe_fields(data)
        elif data[:8] == b"!<arch>\n":
            yield from ar_fields(data)
        elif len(data) >= 32:
            yield from macho_fields(data)
    except (struct.error, ValueError, UnicodeDecodeError):
        return  # not the binary its magic claimed: the text scan still covers it


def zip_entries(archive, prefix=""):
    """zip members, and the members of every .jar inside (an AAR's classes.jar)."""
    for info in archive.infolist():
        stamps = []
        y, mo, d, h, mi, s = info.date_time
        local = dt.datetime(y, mo, d, h, mi, s)
        stamps.append(("zip entry date", local.replace(tzinfo=dt.timezone.utc).timestamp()))
        extra = info.extra
        while len(extra) >= 4:
            tag, size = struct.unpack_from("<HH", extra)
            if tag == 0x5455 and size >= 5 and extra[4] & 1:
                stamps.append(("zip extended mtime", u32(extra, 5)))
            extra = extra[4 + size:]
        data = b"" if info.is_dir() else archive.read(info)
        yield prefix + info.filename, stamps, data
        if info.filename.endswith(".jar"):
            with zipfile.ZipFile(io.BytesIO(data)) as nested:
                yield from zip_entries(nested, f"{prefix}{info.filename}!/")


def entries(path):
    """(name, mtime or None, extra stamps, data) for every member of an asset."""
    if path.endswith((".zip", ".aar")):
        with zipfile.ZipFile(path) as archive:
            yield from zip_entries(archive)
        return
    if path.endswith(".zst"):
        raw = subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout
        archive = tarfile.open(fileobj=io.BytesIO(raw))
    else:
        archive = tarfile.open(path)
    with archive:
        for member in archive.getmembers():
            data = archive.extractfile(member).read() if member.isfile() else b""
            yield member.name, [("tar mtime", member.mtime)], data


def scan(path):
    """(member, where, how, text) for every build clock in an asset."""
    found = []
    count = 0
    for name, stamps, data in entries(path):
        count += 1
        for field, value in stamps:
            day = clock(int(value))
            if day:
                found.append((name, field, f"{day}", f"{int(value)}"))
        for field, offset, value in header_fields(data):
            day = clock(value)
            if day:
                found.append((name, f"@0x{offset:x}", f"{field}, {day}", f"{value}"))
        for offset, how, day, text in text_stamps(data):
            found.append((name, f"@0x{offset:x}", f"{how}, {day}", text))
    return found, count


# Synthetic assets for the self-test.
def tarball(path, files, mtime=EPOCH):
    with tarfile.open(path, "w:gz") as archive:
        for name, payload in files.items():
            info = tarfile.TarInfo(name)
            info.size, info.mtime = len(payload), mtime
            archive.addfile(info, io.BytesIO(payload))


def zipped(path, files, date=(2020, 1, 1, 0, 0, 0)):
    with zipfile.ZipFile(path, "w") as archive:
        for name, payload in files.items():
            archive.writestr(zipfile.ZipInfo(name, date), payload)


def pe_image(stamp, debug_stamp):
    """A PE32+ image with one section holding a debug directory entry."""
    image = bytearray(0x400)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x44, 0x8664, 1, stamp, 0, 0, 240, 0x22)
    optional = 0x58
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<I", image, optional + 108, 16)
    struct.pack_into("<II", image, optional + 112 + 8 * 6, 0x1000, 28)
    table = optional + 240
    image[table:table + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", image, table + 8, 0x100, 0x1000, 0x100, 0x200)
    struct.pack_into("<IIHHIIII", image, 0x200, 0, debug_stamp, 0, 0, 2, 0, 0, 0)
    return bytes(image)


def ar_archive(mtime):
    body = struct.pack("<HHIIIHH", 0x8664, 0, mtime, 0, 0, 0, 0)
    header = f"{'probe.o/':<16}{mtime:<12}{0:<6}{0:<6}{644:<8}{len(body):<10}`\n".encode()
    return b"!<arch>\n" + header + body


def macho_dylib(stamp):
    name = b"@rpath/Probe.framework/Probe\0\0\0\0"
    command = struct.pack("<IIIIII", 0xD, 24 + len(name), 24, stamp, 0x10000, 0x10000) + name
    return struct.pack("<IIIIIIII", 0xFEEDFACF, 0x100000C, 0, 6, 1, len(command), 0, 0) + command


def formats(found):
    return {hit[2].split(",")[0] for hit in found}


# Self-test: the scan has to see each stamp it claims, on the latest build day
# and (by default) on one weeks back, as a cached build tree keeps it; and
# nothing in an asset built with the drivers' fixed epoch.
with tempfile.TemporaryDirectory() as tmp:
    probes = days[-1:] if days else [today, today - dt.timedelta(days=40)]
    for day in probes:
        moment = dt.datetime.combine(day, dt.time(12, 39, 4), dt.timezone.utc)
        seconds = int(moment.timestamp())
        b, a = MONTHS[day.month - 1], WEEKDAYS[day.weekday()]
        spellings = {
            "__DATE__": f"{b} {day.day:2d} {day.year}\0{moment:%H:%M:%S}",
            "asctime": f"built on: {a} {b} {day.day:2d} {moment:%H:%M:%S} {day.year} UTC",
            "ISO 8601": f"{moment:%Y-%m-%d %H:%M:%S}",
            "RFC 2822": f"{a}, {day.day:02d} {b} {day.year} {moment:%H:%M:%S} +0000",
        }
        for name, text in spellings.items():
            path = Path(tmp) / f"{name}.tar.gz"
            tarball(path, {"lib/libprobe.so": b"\x7fELF\0\0" + text.encode() + b"\0\x01\x02"})
            found, _ = scan(str(path))
            check(name in formats(found), f"self-test: a {name} stamp {text!r} on {day} is not detected ({sorted(formats(found))})")
        binaries = {
            "PE TimeDateStamp": pe_image(seconds, 0),
            "debug directory TimeDateStamp": pe_image(0, seconds),
            "ar member 'probe.o/' mtime": ar_archive(seconds),
            "Mach-O dylib timestamp": macho_dylib(seconds),
        }
        for name, payload in binaries.items():
            path = Path(tmp) / "binary.zip"
            zipped(path, {"lib/probe": payload})
            found, _ = scan(str(path))
            check(name in formats(found), f"self-test: a {name} of {day} is not detected ({sorted(formats(found))})")
        path = Path(tmp) / "mtime.tar.gz"
        tarball(path, {"include/probe.h": b"\n"}, mtime=seconds)
        check("tar mtime" in {hit[1] for hit in scan(str(path))[0]}, f"self-test: a tar mtime of {day} is not detected")
        path = Path(tmp) / "mtime.zip"
        zipped(path, {"include/probe.h": b"\n"}, date=(day.year, day.month, day.day, 12, 0, 0))
        check("zip entry date" in {hit[1] for hit in scan(str(path))[0]}, f"self-test: a zip date of {day} is not detected")
        # An AAR: the classes.jar inside is searched too.
        jar = Path(tmp) / "classes.jar"
        zipped(jar, {"com/example/Probe.class": b"\n"}, date=(day.year, day.month, day.day, 12, 0, 0))
        path = Path(tmp) / "probe.aar"
        zipped(path, {"classes.jar": jar.read_bytes()})
        check(
            "classes.jar!/com/example/Probe.class" in {hit[0] for hit in scan(str(path))[0]},
            f"self-test: a zip date of {day} inside an AAR's classes.jar is not detected",
        )
    fixed = (
        b"\x7fELF\0\0built on: Wed Jan  1 00:00:00 2020 UTC\0OpenSSL 4.0.3 1 Oct 2026\0"
        b"Jan  1 2020 00:00:00\0LIB Build date: %s %s\0Jan  1 2020\x0000:00:00\0"
        b"1.6.227 f50435051b72c168\x002020-01-01 00:00:00\0"
    )
    clean = {
        "lib/libprobe.so": fixed,
        "libprobe.dll": pe_image(EPOCH, EPOCH),
        "libprobe-timestamp-0.dll": pe_image(0, 0),
        "libprobe.dll.a": ar_archive(0),
        "Probe.framework/Probe": macho_dylib(1),
    }
    for path in (Path(tmp) / "fixed.tar.gz", Path(tmp) / "fixed.zip"):
        (tarball if path.suffix == ".gz" else zipped)(path, clean)
        found, _ = scan(str(path))
        check(not found, f"self-test: an asset built with the fixed epoch must be clean, found {found}")
if failures:
    sys.exit(f"\n{len(failures)} failure(s): the scan itself is broken")
print(f"self-test: every clock format and header stamp detected, the fixed epoch is clean (build days: {window})")

for path in assets:
    found, count = scan(path)
    for member, where, how, text in found:
        check(False, f"{path}: {member} {where} carries the build clock ({how}): {text!r}")
    if not found:
        print(f"{path}: no build clock in any of {count} entries")

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
PY
