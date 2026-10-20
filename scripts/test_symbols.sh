#!/usr/bin/env bash
# Regression test for scripts/symbols.py, which packs the debug-symbols zips
# a crash tracker pairs with the shipped binaries by identity: the ELF GNU
# build-id, the PE CodeView record and the PDB GUID and age.
#
# The fixtures are synthetic ELF (32/64-bit, both byte orders, extended
# section numbering, with and without a build-id), PE32/PE32+ and MSF 7.00 PDB
# files built byte by byte here, so every assertion is exact on any host. When
# the host links ELF shared objects (a Linux cc with readelf and objcopy), a
# real one is checked against readelf too.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$root" <<'PY'
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
tool = root / "scripts" / "symbols.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(*args, expect=0, env=None, cwd=None):
    environment = dict(os.environ)
    if env:
        environment.update(env)
    result = subprocess.run(
        [sys.executable, str(tool), *map(str, args)],
        capture_output=True, text=True, env=environment, cwd=cwd,
    )
    if expect is not None and result.returncode != expect:
        message = f"symbols.py {' '.join(map(str, args))} exited {result.returncode}, expected {expect}"
        failures.append(f"{message}\n{result.stdout}{result.stderr}")
        print(f"FAIL: {message}\n{result.stdout}{result.stderr}")
    return result


# ---- fixtures -------------------------------------------------------------

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOTE, SHT_NOBITS = 1, 2, 3, 7, 8


def elf(*, bits=64, order="<", build_id=None, lead_note=False, symtab=False,
        dwarf=False, keep_debug=False, extended=False):
    """An ELF shared object's section layout, with exactly the requested parts.

    keep_debug makes .text NOBITS, like `objcopy --only-keep-debug`; lead_note
    puts another note section (Android's ABI note) before the build-id's.
    """

    def note(name, kind, desc):
        name += b"\0"
        padded = lambda data: data + b"\0" * (-len(data) % 4)
        return struct.pack(order + "III", len(name), len(desc), kind) + padded(name) + padded(desc)

    # (name, type, file bytes, size, alignment)
    text = b"\xc3" * 48
    sections = [(".text", SHT_NOBITS if keep_debug else SHT_PROGBITS, b"" if keep_debug else text, len(text), 16)]
    if lead_note:
        ident = note(b"Android", 1, struct.pack(order + "I", 24) + b"r27" + b"\0" * 61)
        sections.append((".note.android.ident", SHT_NOTE, ident, len(ident), 4))
    if build_id is not None:
        data = note(b"GNU", 3, build_id)
        sections.append((".note.gnu.build-id", SHT_NOTE, data, len(data), 4))
    if symtab:
        data = b"\0" * (24 if bits == 64 else 16) * 2
        sections.append((".symtab", SHT_SYMTAB, data, len(data), 8))
        sections.append((".strtab", SHT_STRTAB, b"\0f\0", 3, 1))
    if dwarf:
        data = b"\x2a\0\0\0\x05\0" + b"\x01" * 42
        sections.append((".debug_info", SHT_PROGBITS, data, len(data), 1))
    names = b"\0"
    name_offsets = []
    for name, *_ in sections + [(".shstrtab",)]:
        name_offsets.append(len(names))
        names += name.encode() + b"\0"
    sections.append((".shstrtab", SHT_STRTAB, names, len(names), 1))

    header_size = 64 if bits == 64 else 52
    entry_size = 64 if bits == 64 else 40
    body = bytearray(header_size)
    placed = []
    for name, kind, data, size, align in sections:
        body += b"\0" * (-len(body) % align)
        placed.append((len(body), kind, size, align))
        body += data
    body += b"\0" * (-len(body) % 8)
    shoff = len(body)

    count = len(sections) + 1
    strndx = count - 1
    layout = order + ("IIQQQQIIQQ" if bits == 64 else "IIIIIIIIII")
    # Section 0 carries the real count and string-table index when they are
    # spelled as extended numbering.
    body += struct.pack(layout, 0, 0, 0, 0, 0, count if extended else 0, strndx if extended else 0, 0, 0, 0)
    for name_offset, (offset, kind, size, align) in zip(name_offsets, placed):
        body += struct.pack(layout, name_offset, kind, 0, 0, offset, size, 0, 0, align, 0)

    ident = b"\x7fELF" + bytes([2 if bits == 64 else 1, 1 if order == "<" else 2, 1]) + b"\0" * 9
    header = struct.pack(
        order + ("16sHHIQQQIHHHHHH" if bits == 64 else "16sHHIIIIIHHHHHH"),
        ident, 3, 183 if bits == 64 else 40, 1, 0, 0, shoff, 0,
        header_size, 0, 0, entry_size, 0 if extended else count, 0xFFFF if extended else strndx,
    )
    body[:header_size] = header
    return bytes(body)


IMAGE_DEBUG_TYPE_CODEVIEW, IMAGE_DEBUG_TYPE_REPRO = 2, 16


def pe(*, guid=None, age=1, pe32=False, debug_directory=True):
    """A one-section PE image whose debug directory leads with a REPRO entry,
    then the CodeView record when a GUID is given."""
    lfanew, raw, rva = 0x80, 0x400, 0x1000
    optional_size = (96 if pe32 else 112) + 8 * 16
    payloads = [(IMAGE_DEBUG_TYPE_REPRO, b"")]
    if guid is not None:
        payloads.append((IMAGE_DEBUG_TYPE_CODEVIEW, b"RSDS" + guid + struct.pack("<I", age) + b"C:\\b\\libmpv-2.pdb\0"))
    directory = b""
    data = b""
    data_start = 28 * len(payloads)
    for kind, payload in payloads:
        offset = data_start + len(data)
        directory += struct.pack("<IIHHIIII", 0, 0, 0, 0, kind, len(payload), rva + offset, raw + offset)
        data += payload
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x10B if pe32 else 0x20B)
    directories = 96 if pe32 else 112
    struct.pack_into("<I", optional, directories - 4, 16)
    if debug_directory:
        struct.pack_into("<II", optional, directories + 8 * 6, rva, len(directory))

    image = bytearray(b"MZ" + b"\0" * (lfanew - 2))
    struct.pack_into("<I", image, 0x3C, lfanew)
    image += b"PE\0\0" + struct.pack("<HHIIIHH", 0x14C if pe32 else 0x8664, 1, 0, 0, 0, optional_size, 0x2022)
    image += optional
    image += struct.pack("<8sIIIIIIHHI", b".rdata", 0x200, rva, 0x200, raw, 0, 0, 0, 0, 0x40000040)
    image += b"\0" * (raw - len(image))
    image += (directory + data).ljust(0x200, b"\0")
    return bytes(image)


MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


def pdb(*, guid, info_age=1, dbi_age=None, extra_streams=0, block_size=512):
    """An MSF 7.00 file: the PDB info stream (1), the DBI stream (3) carrying
    dbi_age unless it is None (a nil stream), and extra empty streams that
    spread the stream directory over several blocks."""
    info = struct.pack("<III", 20000404, 0x5EED5EED, info_age) + guid
    dbi = None if dbi_age is None else struct.pack("<iII", -1, 19990903, dbi_age) + b"\0" * 52
    streams = [b"", info, b"", dbi] + [b""] * extra_streams
    blocks = {}
    next_block = 3  # 0 is the superblock, 1 and 2 the free block maps
    stream_blocks = []
    for data in streams:
        used = []
        for start in range(0, len(data or b""), block_size):
            blocks[next_block] = data[start:start + block_size]
            used.append(next_block)
            next_block += 1
        stream_blocks.append(used)
    sizes = [0xFFFFFFFF if data is None else len(data) for data in streams]
    directory = struct.pack(f"<I{len(streams)}I", len(streams), *sizes)
    directory += b"".join(struct.pack(f"<{len(used)}I", *used) for used in stream_blocks)
    directory_blocks = []
    for start in range(0, len(directory), block_size):
        blocks[next_block] = directory[start:start + block_size]
        directory_blocks.append(next_block)
        next_block += 1
    block_map = next_block
    blocks[block_map] = struct.pack(f"<{len(directory_blocks)}I", *directory_blocks)
    blocks[0] = MSF_MAGIC + struct.pack("<6I", block_size, 1, block_map + 1, len(directory), 0, block_map)
    return b"".join(blocks.get(index, b"").ljust(block_size, b"\0") for index in range(block_map + 1))


def debug_id(guid, age):
    return f"{uuid.UUID(bytes_le=guid)}-{age:x}"


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


ID_A = bytes(range(1, 21))
ID_B = bytes(range(101, 121))
ID_C = bytes.fromhex("c0ffee00112233445566778899aabbcc")
ID_D = bytes(range(40, 48))
GUID = bytes.fromhex("ed75b344490d7f3d4c4c44205044422e")
GUID_AGED = bytes.fromhex("70d5015ab886828c4c4c44205044422e")
GUID_OTHER = bytes(range(16))

with tempfile.TemporaryDirectory() as tmp:
    t = Path(tmp)
    # What an Android ABI ships (stripped by AGP) and what its tarball keeps.
    shipped = write(t / "aar/libmpv.so", elf(build_id=ID_A, lead_note=True))
    unstripped = write(t / "tarball/libmpv.so", elf(build_id=ID_A, lead_note=True, symtab=True, dwarf=True))
    symtab_only = write(t / "symtab/libmpv.so", elf(build_id=ID_A, symtab=True))
    keep_debug = write(t / "keep/libmpv.so", elf(build_id=ID_A, symtab=True, dwarf=True, keep_debug=True))
    other = write(t / "other/libmpv.so", elf(build_id=ID_B, symtab=True, dwarf=True))
    no_id = write(t / "noid/libmpv.so", elf(symtab=True, dwarf=True))
    codec_shipped = write(t / "aar/libavcodec.so", elf(bits=32, order="<", build_id=ID_C))
    codec_debug = write(t / "tarball/libavcodec.so", elf(bits=32, order="<", build_id=ID_C, symtab=True, dwarf=True))
    big_endian = write(t / "be/libbe.so", elf(bits=32, order=">", build_id=ID_C, dwarf=True))
    extended = write(t / "ext/libext.so", elf(build_id=ID_D, symtab=True, extended=True))
    dll = write(t / "win/libmpv-2.dll", pe(guid=GUID, age=1))
    pdb_file = write(t / "win/libmpv-2.pdb", pdb(guid=GUID, info_age=1, dbi_age=1, extra_streams=150))
    dll32 = write(t / "win32/libmpv-2.dll", pe(guid=GUID_AGED, age=5, pe32=True))
    aged_pdb = write(t / "win32/aged.pdb", pdb(guid=GUID_AGED, info_age=1, dbi_age=5))
    nodbi_pdb = write(t / "win32/nodbi.pdb", pdb(guid=GUID_AGED, info_age=5))
    other_pdb = write(t / "otherpdb/libmpv-2.pdb", pdb(guid=GUID_OTHER, dbi_age=1))
    no_codeview = write(t / "nocv/libmpv-2.dll", pe())
    no_directory = write(t / "nodir/libmpv-2.dll", pe(guid=GUID, debug_directory=False))
    text = write(t / "notes.txt", b"not a binary\n")
    truncated = write(t / "truncated.so", elf(build_id=ID_A)[:40])

    # 1. id: every format's identity, spelled the way the tracker spells it.
    result = run("id", shipped, keep_debug, codec_shipped, big_endian, extended, dll, pdb_file, dll32, aged_pdb, nodbi_pdb)
    expected = [
        f"{ID_A.hex()}  {shipped}",
        f"{ID_A.hex()}  {keep_debug}",
        f"{ID_C.hex()}  {codec_shipped}",
        f"{ID_C.hex()}  {big_endian}",
        f"{ID_D.hex()}  {extended}",
        f"{debug_id(GUID, 1)}  {dll}",
        f"{debug_id(GUID, 1)}  {pdb_file}",
        f"{debug_id(GUID_AGED, 5)}  {dll32}",
        f"{debug_id(GUID_AGED, 5)}  {aged_pdb}",
        f"{debug_id(GUID_AGED, 5)}  {nodbi_pdb}",
    ]
    check(result.stdout.splitlines() == expected, f"id output:\n{result.stdout}")
    check(
        debug_id(GUID, 1) == "44b375ed-0d49-3d7f-4c4c-44205044422e-1",
        "the GUID is spelled with its first three fields little-endian",
    )
    result = run("id", no_id, no_codeview, no_directory, text, expect=1)
    check(
        all(str(path) in result.stderr for path in (no_id, no_codeview, no_directory, text))
        and str(no_id) not in result.stdout,
        f"id names every file without an identity: {result.stderr}",
    )
    result = run("id", shipped, no_id, expect=1)
    check(f"{ID_A.hex()}  {shipped}" in result.stdout, "id still prints the identities it found")
    result = run("id", truncated, expect=1)
    check("unreadable as ELF, PE or PDB" in result.stderr, f"a truncated ELF is reported: {result.stderr}")
    print("id: ELF build-ids, PE CodeView records and PDB GUID+DBI age all read")

    # 2. match: the candidate with the shipped identity and the most symbols.
    result = run("match", shipped, other, shipped, symtab_only, keep_debug, unstripped)
    check(result.stdout.strip() == str(keep_debug), f"DWARF beats a symbol table, first among equals: {result.stdout}")
    result = run("match", shipped, symtab_only, unstripped)
    check(result.stdout.strip() == str(unstripped), f"DWARF beats an earlier symbol table: {result.stdout}")
    result = run("match", shipped, shipped, other, symtab_only)
    check(result.stdout.strip() == str(symtab_only), f"a symbol table beats nothing: {result.stdout}")
    result = run("match", shipped, shipped, other, no_id, expect=1)
    check(f"no candidate has its identity {ID_A.hex()}" in result.stderr, f"no match: {result.stderr}")
    result = run("match", no_id, unstripped, expect=1)
    check("no debug identity" in result.stderr, f"a shipped file without an identity: {result.stderr}")
    result = run("match", dll, other_pdb, pdb_file)
    check(result.stdout.strip() == str(pdb_file), f"the PDB with the DLL's GUID: {result.stdout}")
    result = run("match", dll32, nodbi_pdb, aged_pdb)
    check(result.stdout.strip() == str(nodbi_pdb), f"an age-5 PDB matches the age-5 DLL: {result.stdout}")
    run("match", dll, aged_pdb, expect=1)
    print("match: identity first, then debug info over a symbol table over nothing")

    # 3. pack: checked identities, a deterministic zip at the archive root.
    pairs = [
        ("--pair", shipped, unstripped),
        ("--pair", codec_shipped, codec_debug),
        ("--pair", dll, pdb_file),
    ]
    first = t / "out" / "first.zip"
    result = run("pack", first, *[arg for pair in pairs for arg in pair])
    check(f"packed {first.name}" in result.stdout, f"pack reports the zip: {result.stdout}")
    with zipfile.ZipFile(first) as archive:
        infos = archive.infolist()
        check(
            [info.filename for info in infos] == ["libavcodec.so", "libmpv-2.pdb", "libmpv.so"],
            f"members are the debug files' names at the root, in byte order: {archive.namelist()}",
        )
        for info, source in zip(infos, (codec_debug, pdb_file, unstripped)):
            check(archive.read(info) == source.read_bytes(), f"{info.filename}: the debug file's bytes")
            check(info.date_time == (2020, 1, 1, 0, 0, 0), f"{info.filename}: date {info.date_time}")
            check(info.external_attr >> 16 == 0o100644, f"{info.filename}: mode {oct(info.external_attr >> 16)}")
            check(info.extra == b"", f"{info.filename}: no extra attributes, got {info.extra!r}")
            check(info.compress_type == zipfile.ZIP_DEFLATED, f"{info.filename}: deflated")

    # Pair order, source mtimes and modes, the output's previous bytes and the
    # time zone must not reach the zip.
    for path in (unstripped, codec_debug, pdb_file):
        os.utime(path, (1_700_000_000, 1_700_000_000))
        path.chmod(0o755)
    second = write(t / "out" / "second.zip", b"previous bytes")
    run("pack", second, *[arg for pair in reversed(pairs) for arg in pair], env={"TZ": "Pacific/Auckland"})
    third = t / "out" / "third.zip"
    run("pack", third, *[arg for pair in pairs for arg in pair], env={"TZ": "UTC"})
    digests = {hashlib.sha256(path.read_bytes()).hexdigest() for path in (first, second, third)}
    check(len(digests) == 1, f"pack is byte-reproducible: {len(digests)} distinct zips")

    # The build-date gate every release asset passes accepts the stamp.
    gate = subprocess.run(["bash", str(root / "scripts" / "test_build_date.sh"), str(first)], capture_output=True, text=True)
    check(gate.returncode == 0, f"test_build_date.sh rejects a symbols zip:\n{gate.stdout}{gate.stderr}")

    refused = write(t / "out" / "refused.zip", b"untouched")
    for pair, needle in (
        ((shipped, other), f"identity {ID_B.hex()} is not {shipped}'s {ID_A.hex()}"),
        ((shipped, no_id), f"identity None is not {shipped}'s"),
        ((dll, other_pdb), f"is not {dll}'s {debug_id(GUID, 1)}"),
        ((dll32, aged_pdb.with_name("missing.pdb")), "unreadable as ELF, PE or PDB"),
        ((shipped, shipped), "carries no symbols"),
        ((no_id, no_id), "no debug identity"),
        ((no_codeview, pdb_file), "no debug identity"),
    ):
        result = run("pack", refused, "--pair", *pair, expect=1)
        check(needle in result.stderr, f"pack refuses {pair[1]} for {pair[0]}: {result.stderr.strip()}")
        check(refused.read_bytes() == b"untouched", "a refused pack leaves the output alone")
    result = run("pack", refused, "--pair", shipped, unstripped, "--pair", shipped, keep_debug, expect=1)
    check("two debug files would share one archive name" in result.stderr, f"duplicate names: {result.stderr}")
    check(refused.read_bytes() == b"untouched", "a refused pack leaves the output alone")
    result = run("pack", t / "out" / "partial.zip", "--pair", codec_shipped, codec_debug, "--pair", shipped, other, expect=1)
    check(not (t / "out" / "partial.zip").exists(), "one bad pair writes no zip at all")
    print("pack: identities checked, members at the root, bytes reproducible")

    # 4. A real linker's output, when this host links ELF shared objects.
    tools = {name: shutil.which(name) for name in ("cc", "readelf", "objcopy")}
    real = t / "real"
    real.mkdir()
    (real / "real.c").write_text("int real_answer(int x) { return x * 42; }\n", encoding="utf-8")

    def link(output, build_id):
        command = [tools["cc"], "-shared", "-fPIC", "-g", "-O1", f"-Wl,--build-id={build_id}",
                   "-o", str(output), str(real / "real.c")]
        linked = subprocess.run(command, capture_output=True)
        return linked.returncode == 0 and output.read_bytes()[:4] == b"\x7fELF"

    if not all(tools.values()) or not link(real / "libreal.so", "sha1"):
        print(f"real ELF: skipped (no cc/readelf/objcopy linking ELF here: {tools})")
    else:
        notes = subprocess.run([tools["readelf"], "-n", str(real / "libreal.so")], capture_output=True, text=True, check=True).stdout
        build_id = next(line.split(":", 1)[1].strip() for line in notes.splitlines() if "Build ID:" in line)
        debug = real / "debug" / "libreal.so"
        stripped = real / "stripped" / "libreal.so"
        debug.parent.mkdir()
        stripped.parent.mkdir()
        subprocess.run([tools["objcopy"], "--only-keep-debug", str(real / "libreal.so"), str(debug)], check=True)
        subprocess.run([tools["objcopy"], "--strip-all", str(real / "libreal.so"), str(stripped)], check=True)
        result = run("id", real / "libreal.so", debug, stripped)
        check(
            [line.split()[0] for line in result.stdout.splitlines()] == [build_id] * 3,
            f"readelf's build-id {build_id} for the linked, debug and stripped copies: {result.stdout}",
        )
        result = run("match", stripped, stripped, debug)
        check(result.stdout.strip() == str(debug), f"the --only-keep-debug copy matches: {result.stdout}")
        run("pack", real / "real.zip", "--pair", stripped, debug)
        check(link(real / "libnoid.so", "none"), "cc links without a build-id")
        result = run("id", real / "libnoid.so", expect=1)
        check("no debug identity" in result.stderr, "a real ELF without a build-id has no identity")
        print(f"real ELF: readelf agrees ({build_id}); objcopy's debug copy matches and packs")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)

print("\nall checks passed")
PY
