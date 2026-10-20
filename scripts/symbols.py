#!/usr/bin/env python3
"""Debug-symbol assets: one zip of debug files per shipped variant.

Usage:
  symbols.py id <file>...
  symbols.py match <shipped> <candidate>...
  symbols.py pack <output.zip> --pair <shipped> <debug> [--pair <shipped> <debug>]...

A crash tracker (Sentry and the self-hosted tracker Plezy reports to) pairs a
debug file with the binary a crash names by identity, never by file name:

  ELF  the GNU build-id note (NT_GNU_BUILD_ID), spelled as its hex code id. A
       binary without one gets an id hashed from its first page of .text,
       which a `--only-keep-debug` file (whose .text is NOBITS) cannot
       reproduce, so every shipped ELF has to carry the note.
  PE   the CodeView (RSDS) record in the debug directory: GUID and age,
       spelled as the tracker's debug id, <guid>-<age in hex>.
  PDB  the GUID of the PDB info stream and the age of the DBI stream (the
       age the image records), spelled the same way.

`id` prints `<identity>  <file>` for each file and fails when any of them has
no identity. `match` prints the first candidate that has <shipped>'s identity
and symbols, preferring DWARF over a bare symbol table; the AAR build finds the
unstripped copy of each library it ships this way. `pack` checks that every
<debug> has the identity of its <shipped> binary and symbols to go with it,
then zips the debug files at the archive root under their own names.

The zip is a content-addressed release asset, so a rebuild of the same key has
to reproduce its bytes. Every member is copied, given mode 0644 and an mtime
of 2020-01-01 00:00 local time (the `touch -t 202001010000` stamp every other
zip here carries; zip stores the local DOS time), and zipped in byte order with
no extra attributes (-X). This file writes keyed bytes, so keys.py keys every
artifact whose symbols it packs on it.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"

SHT_SYMTAB = 2
SHT_NOTE = 7
SHT_NOBITS = 8
NT_GNU_BUILD_ID = 3
IMAGE_DEBUG_TYPE_CODEVIEW = 2
# The data directory index of the debug directory.
IMAGE_DIRECTORY_ENTRY_DEBUG = 6
# PDB streams: the info stream (version, signature, age, GUID) and the DBI
# stream, whose header carries the age the image's CodeView record holds.
PDB_INFO_STREAM = 1
PDB_DBI_STREAM = 3

# How much symbol information a file has: none, a symbol table (function
# names), or debug info (DWARF or a PDB: names, lines and inlining).
NO_SYMBOLS, SYMBOL_TABLE, DEBUG_INFO = 0, 1, 2
DWARF_SECTIONS = (".debug_info", ".zdebug_info")

NORMALIZED_MODE = 0o644
# 2020-01-01 00:00 local time: zip stores the local DOS time, so this is what
# `touch -t 202001010000` gives the other zips in this repository.
NORMALIZED_MTIME = time.mktime((2020, 1, 1, 0, 0, 0, 0, 0, -1))


def _debug_id(guid: bytes, age: int) -> str:
    data1, data2, data3 = struct.unpack_from("<IHH", guid)
    return f"{data1:08x}-{data2:04x}-{data3:04x}-{guid[8:10].hex()}-{guid[10:16].hex()}-{age:x}"


def _read_at(handle, offset: int, size: int) -> bytes:
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise ValueError("truncated")
    return data


def _elf(handle) -> tuple[str | None, int]:
    header = _read_at(handle, 0, 64)
    bits = {1: 32, 2: 64}.get(header[4])
    order = {1: "<", 2: ">"}.get(header[5])
    if bits is None or order is None:
        raise ValueError("unknown ELF class or byte order")
    if bits == 64:
        (shoff,) = struct.unpack_from(order + "Q", header, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", header, 0x3A)
        layout = order + "IIQQQQIIQQ"
    else:
        (shoff,) = struct.unpack_from(order + "I", header, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(order + "HHH", header, 0x2E)
        layout = order + "IIIIIIIIII"
    if not shoff:
        return None, NO_SYMBOLS
    # (name, type, offset, size, link, addralign) of every section header.
    def section(index: int) -> tuple[int, ...]:
        fields = struct.unpack(layout, _read_at(handle, shoff + index * shentsize, struct.calcsize(layout)))
        return fields[0], fields[1], fields[4], fields[5], fields[6], fields[8]

    # Extended numbering: a count or string-table index that does not fit
    # the header lives in section 0.
    if shnum == 0:
        shnum = section(0)[3]
    if shstrndx == 0xFFFF:
        shstrndx = section(0)[4]
    sections = [section(index) for index in range(shnum)]
    _, _, names_offset, names_size, _, _ = sections[shstrndx]
    names = _read_at(handle, names_offset, names_size)

    build_id = None
    symbols = NO_SYMBOLS
    for name_offset, kind, offset, size, _, align in sections:
        name = names[name_offset:names.index(b"\0", name_offset)].decode("ascii", "replace")
        if kind == SHT_NOBITS or not size:
            continue
        if name in DWARF_SECTIONS:
            symbols = DEBUG_INFO
        elif kind == SHT_SYMTAB:
            symbols = max(symbols, SYMBOL_TABLE)
        elif kind == SHT_NOTE and build_id is None:
            build_id = _gnu_build_id(_read_at(handle, offset, size), order, 8 if align == 8 else 4)
    return build_id, symbols


def _gnu_build_id(notes: bytes, order: str, align: int) -> str | None:
    def padded(size: int) -> int:
        return (size + align - 1) // align * align

    position = 0
    while position + 12 <= len(notes):
        name_size, desc_size, kind = struct.unpack_from(order + "III", notes, position)
        name_start = position + 12
        desc_start = name_start + padded(name_size)
        name = notes[name_start:name_start + name_size].rstrip(b"\0")
        if name == b"GNU" and kind == NT_GNU_BUILD_ID and desc_size:
            return notes[desc_start:desc_start + desc_size].hex()
        position = desc_start + padded(desc_size)
    return None


def _pe(handle) -> tuple[str | None, int]:
    (lfanew,) = struct.unpack("<I", _read_at(handle, 0x3C, 4))
    if _read_at(handle, lfanew, 4) != b"PE\0\0":
        return None, NO_SYMBOLS
    _, section_count, _, _, _, optional_size, _ = struct.unpack("<HHIIIHH", _read_at(handle, lfanew + 4, 20))
    optional = _read_at(handle, lfanew + 24, optional_size)
    (magic,) = struct.unpack_from("<H", optional)
    directories = {0x10B: 96, 0x20B: 112}.get(magic)
    if directories is None:
        raise ValueError(f"unknown PE optional header magic {magic:#x}")
    (directory_count,) = struct.unpack_from("<I", optional, directories - 4)
    if directory_count <= IMAGE_DIRECTORY_ENTRY_DEBUG:
        return None, NO_SYMBOLS
    rva, size = struct.unpack_from("<II", optional, directories + 8 * IMAGE_DIRECTORY_ENTRY_DEBUG)
    table = _read_at(handle, lfanew + 24 + optional_size, 40 * section_count)
    offset = None
    for index in range(section_count):
        virtual_size, address, raw_size, raw_pointer = struct.unpack_from("<IIII", table, index * 40 + 8)
        if address <= rva < address + max(virtual_size, raw_size):
            offset = raw_pointer + rva - address
            break
    if not rva or offset is None:
        return None, NO_SYMBOLS
    directory = _read_at(handle, offset, size)
    for entry in range(0, size - size % 28, 28):
        kind, data_size, _, data_pointer = struct.unpack_from("<IIII", directory, entry + 12)
        if kind != IMAGE_DEBUG_TYPE_CODEVIEW or data_size < 24:
            continue
        record = _read_at(handle, data_pointer, 24)
        if record[:4] == b"RSDS":
            return _debug_id(record[4:20], struct.unpack_from("<I", record, 20)[0]), NO_SYMBOLS
    return None, NO_SYMBOLS


def _pdb(handle) -> tuple[str | None, int]:
    block_size, _, _, directory_bytes, _, block_map = struct.unpack(
        "<6I", _read_at(handle, len(MSF_MAGIC), 24)
    )
    count = -(-directory_bytes // block_size)
    directory_blocks = struct.unpack(f"<{count}I", _read_at(handle, block_map * block_size, 4 * count))
    directory = b"".join(_read_at(handle, block * block_size, block_size) for block in directory_blocks)
    (stream_count,) = struct.unpack_from("<I", directory)
    sizes = [
        0 if size == 0xFFFFFFFF else size
        for size in struct.unpack_from(f"<{stream_count}I", directory, 4)
    ]
    blocks = []
    position = 4 + 4 * stream_count
    for size in sizes:
        used = -(-size // block_size)
        blocks.append(struct.unpack_from(f"<{used}I", directory, position))
        position += 4 * used

    def stream(index: int, length: int) -> bytes | None:
        """The first `length` bytes of a stream, or None when it is shorter."""
        if index >= stream_count or sizes[index] < length:
            return None
        used = blocks[index][: -(-length // block_size)]
        return b"".join(_read_at(handle, block * block_size, block_size) for block in used)[:length]

    info = stream(PDB_INFO_STREAM, 28)
    if info is None:
        return None, NO_SYMBOLS
    _, _, age = struct.unpack_from("<III", info)
    dbi = stream(PDB_DBI_STREAM, 12)
    if dbi is not None:
        (age,) = struct.unpack_from("<I", dbi, 8)
    return _debug_id(info[12:28], age), DEBUG_INFO


def inspect(path: str | Path) -> tuple[str | None, int]:
    """(identity or None, symbol level) of an ELF, PE or PDB file."""
    try:
        with open(path, "rb") as handle:
            magic = handle.read(len(MSF_MAGIC))
            if magic.startswith(ELF_MAGIC):
                return _elf(handle)
            if magic.startswith(b"MZ"):
                return _pe(handle)
            if magic == MSF_MAGIC:
                return _pdb(handle)
    except (OSError, ValueError, struct.error, IndexError) as error:
        raise SystemExit(f"{path}: unreadable as ELF, PE or PDB ({error})")
    return None, NO_SYMBOLS


def _identity(path: str | Path) -> str:
    identity, _ = inspect(path)
    if identity is None:
        raise SystemExit(
            f"{path}: no debug identity (an ELF needs a GNU build-id note, a PE a "
            "CodeView record); a crash in it could never be matched to symbols"
        )
    return identity


def cmd_id(args) -> int:
    missing = []
    for path in args.files:
        identity, _ = inspect(path)
        if identity is None:
            missing.append(path)
        else:
            print(f"{identity}  {path}")
    if missing:
        raise SystemExit(
            "no debug identity (GNU build-id note or CodeView record) in:\n  " + "\n  ".join(missing)
        )
    return 0


def cmd_match(args) -> int:
    wanted = _identity(args.shipped)
    best = None
    for candidate in args.candidates:
        identity, symbols = inspect(candidate)
        if identity == wanted and symbols and (best is None or symbols > best[1]):
            best = (candidate, symbols)
    if best is None:
        raise SystemExit(f"{args.shipped}: no candidate has its identity {wanted} and symbols")
    print(best[0])
    return 0


def cmd_pack(args) -> int:
    members: dict[str, Path] = {}
    for shipped, debug in args.pair:
        wanted = _identity(shipped)
        identity, symbols = inspect(debug)
        if identity != wanted:
            raise SystemExit(f"{debug}: identity {identity} is not {shipped}'s {wanted}")
        if not symbols:
            raise SystemExit(f"{debug}: carries no symbols (no DWARF, symbol table or PDB)")
        name = Path(debug).name
        if name in members:
            raise SystemExit(f"{name}: two debug files would share one archive name")
        members[name] = Path(debug)
        print(f"{identity}  {name}")

    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        stage = Path(temporary) / "stage"
        stage.mkdir()
        for name, path in members.items():
            copy = stage / name
            shutil.copyfile(path, copy)
            os.chmod(copy, NORMALIZED_MODE)
            os.utime(copy, (NORMALIZED_MTIME, NORMALIZED_MTIME))
        partial = Path(temporary) / "symbols.zip"
        listing = b"".join(name + b"\n" for name in sorted(os.fsencode(name) for name in members))
        subprocess.run(["zip", "-q", "-X", "-9", str(partial), "-@"], cwd=stage, input=listing, check=True)
        output.unlink(missing_ok=True)
        shutil.move(partial, output)
    print(f"packed {output.name}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)

    identify = subparsers.add_parser("id", help="print each file's debug identity")
    identify.add_argument("files", nargs="+")
    identify.set_defaults(func=cmd_id)

    match = subparsers.add_parser("match", help="print the candidate with <shipped>'s identity and symbols")
    match.add_argument("shipped")
    match.add_argument("candidates", nargs="+")
    match.set_defaults(func=cmd_match)

    pack = subparsers.add_parser("pack", help="zip debug files whose identity matches their shipped binary")
    pack.add_argument("output")
    pack.add_argument("--pair", nargs=2, action="append", required=True, metavar=("SHIPPED", "DEBUG"))
    pack.set_defaults(func=cmd_pack)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
