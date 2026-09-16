#!/usr/bin/env python3
"""Pull production C out of a patch or a patched tree, for the host harnesses.

Every test_*.sh driver needs some of the same few operations, and each used to
carry its own copy of one or more of them. One implementation, four operations,
each writing its output to a file (`--append` to concatenate in order):

  post-image <patch> <out>
      The patch's post-image: its added lines, plus its context lines.

  region <text> <out> --start M [--patch] [--added-only] [--require S]...
      The lines between `// --- M` and `// --- end M`. `--patch` reads a patch
      file (its post-image); without it the text file's own lines, which is what
      slicer harnesses reading an already-patched tree want. `--added-only`
      keeps just a patch's added lines, for a region wholly inside new code.

  hunks <patch> <out.json>
      Every hunk as {"func": <enclosing function from the @@ header>,
      "post": [...], "added": [...]}, for the structural drivers that have to
      assert on a patch no behavioural test can reach.

  range <text> <out> --first A [--last B] [--last-inclusive]
      The lines from the one line containing A up to the one containing B
      (exclusive unless --last-inclusive). For regions bounded by whatever code
      happens to neighbor them.

  define <text> <out> --name N [--name N]...
      Whole `#define N ...` lines, including backslash continuations.

  type <text> <out> [--name N]... [--member M]...
      Whole `struct N { ... };` or `enum N { ... };` definitions, plus unnamed
      ones (`enum { M, ... };`) anchored by their first member.

  symbol <text> <out> --fn N [--fn N]... [--return T]
      Whole `static ... N(...) { ... }` definitions. Definitions only: a
      declaration or a forward use is rejected instead of extracting an empty
      body. `--return` pins the return type when a name is ambiguous.

Everything fails loudly and names what was missing. A harness that silently
compiles a different region than it claims is worse than one that does not run.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def read(path: Path) -> str:
    if not path.is_file():
        fail(f"{path}: no such file")
    return path.read_text(encoding="utf-8")


def post_image(patch: Path) -> list[str]:
    """The patch's post-image: added lines plus context lines.

    Hunk headers and the ---/+++ file markers are never part of it. An empty
    post-image is a failure rather than an empty result: a pure deletion has
    one, and so does a file that is not a diff at all, and writing either out
    as an empty output file would report success for a chunk the harnesses go
    on to compile.
    """
    lines = []
    for raw in read(patch).splitlines():
        if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
            continue
        if raw.startswith(("+", " ")):
            lines.append(raw[1:])
    if not lines:
        fail(f"{patch}: the post-image is empty; is this a diff?")
    return lines


def unique(lines: list[str], pattern: re.Pattern, what: str, origin: Path) -> int:
    matches = [i for i, line in enumerate(lines) if pattern.match(line)]
    if len(matches) != 1:
        fail(f"{origin}: expected exactly one {what}, found {len(matches)}")
    return matches[0]


def conditional_contexts(text: str) -> list[tuple[tuple[int, int], ...]]:
    """Per line, the enclosing `#if` frames: (frame id, which arm).

    ffmpeg compiles one arm of every `#if`/`#else` chain, so two definitions of
    the same name in different arms are alternatives, not a redefinition. The
    ids are assigned in nesting order, which makes the frames of two lines
    comparable position by position.
    """
    frames: list[tuple[int, int]] = []
    contexts = []
    opened = 0
    for line in text.splitlines():
        stripped = line.lstrip()
        contexts.append(tuple(frames))
        if stripped.startswith("#if"):
            frames.append((opened, 0))
            opened += 1
        elif stripped.startswith(("#else", "#elif")):
            if frames:
                frame, arm = frames[-1]
                frames[-1] = (frame, arm + 1)
        elif stripped.startswith("#endif"):
            if frames:
                frames.pop()
    return contexts


def mutually_exclusive(one: tuple[tuple[int, int], ...], other: tuple[tuple[int, int], ...]) -> bool:
    """Whether two lines sit in arms that never compile together."""
    for (frame, arm), (other_frame, other_arm) in zip(one, other):
        if frame != other_frame:
            break
        if arm != other_arm:
            return True
    return False


def extract_region(source: Path, marker: str, added_only: bool, patch: bool) -> list[str]:
    """The lines from the `// --- <marker>` line through its `// --- end` twin.

    Matched as a substring of the line, so a marker may carry a trailing note;
    each must still match exactly one line.
    """
    start, end = f"// --- {marker}", f"// --- end {marker}"
    if patch:
        lines = post_image(source)
        if added_only:
            # The caller wants added lines only, so drop the context lines the
            # post-image carries.
            lines = [line for line, raw in zip(lines, _raw_post_image(source)) if raw.startswith("+")]
    else:
        lines = read(source).splitlines()
        if added_only:
            fail(f"{source}: --added-only only means something with --patch")

    def find(needle: str) -> int:
        hits = [i for i, line in enumerate(lines) if needle in line]
        if len(hits) != 1:
            fail(f"{source}: expected exactly one {needle!r} line, found {len(hits)}")
        return hits[0]

    first, last = find(start), find(end)
    if last < first:
        fail(f"{source}: {end!r} precedes {start!r}")
    return lines[first : last + 1]


def extract_range(source: Path, first: str, last: str | None, inclusive: bool) -> list[str]:
    """The lines from the one containing `first` through the one containing `last`.

    `last` is exclusive by default, which is what "stop before this declaration"
    wants; `inclusive` keeps it, for "through the end of this statement".
    """
    lines = read(source).splitlines()
    starts = [i for i, line in enumerate(lines) if first in line]
    if len(starts) != 1:
        fail(f"{source}: expected exactly one line containing {first!r}, found {len(starts)}")
    if last is None:
        return lines[starts[0] :]
    ends = [i for i, line in enumerate(lines) if last in line]
    if len(ends) != 1:
        fail(f"{source}: expected exactly one line containing {last!r}, found {len(ends)}")
    if ends[0] < starts[0]:
        fail(f"{source}: {last!r} precedes {first!r}")
    return lines[starts[0] : ends[0] + (1 if inclusive else 0)]


def _raw_post_image(patch: Path) -> list[str]:
    raws = []
    for raw in read(patch).splitlines():
        if raw.startswith(("+++", "---", "diff ", "index ", "@@")):
            continue
        if raw.startswith(("+", " ")):
            raws.append(raw)
    return raws


def extract_defines(text: str, origin: Path, names: list[str], out: list[str]) -> None:
    lines = text.splitlines()
    for name in names:
        start = unique(lines, re.compile(r"#define " + re.escape(name) + r"\b"), f"#define {name}", origin)
        end = start
        # A continued macro is one definition: keep its backslashes intact, so
        # the chunk must be emitted as one block, never line by line. A
        # definition that runs off the end of the file is a truncated or
        # malformed source, so name it rather than walking past the list.
        while lines[end].rstrip().endswith("\\"):
            end += 1
            if end >= len(lines):
                fail(f"{origin}: the #define {name} never ends: its last line is a continuation")
        out.append("\n".join(lines[start : end + 1]))


def extract_types(text: str, origin: Path, names: list[str], out: list[str]) -> None:
    lines = text.splitlines()
    for name in names:
        start = unique(
            lines, re.compile(r"^(?:typedef )?(?:struct|enum) " + re.escape(name) + r"\b"), f"{name} definition", origin
        )
        close = next(
            (i for i in range(start, len(lines)) if lines[i] in ("};", "} ;")),
            None,
        )
        if close is None:
            fail(f"{origin}: {name} definition never closes")
        out.append("\n".join(lines[start : close + 1]))


def extract_hunks(patch: Path) -> list[dict[str, object]]:
    """Every hunk of a patch as {func, post, added}.

    `func` is the trailing context of the `@@` header, which names the function
    the hunk falls in -- the reliable anchor when a diff carries only a few
    lines of context and a whole function is rarely present in one piece.
    `post` is the hunk's post-image (added plus context lines) and `added` only
    its added lines.

    A hunk ends where its header says it does. Anything after that belongs to
    no hunk, which is what keeps `git format-patch`'s trailer -- notably the
    blank line before its `-- \\n<version>` signature -- out of the last hunk's
    post-image. Post-image is what the harnesses compile, so absorbing a line
    the hunk does not have hands them a chunk the patch never wrote.
    """
    hunks: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    old_left = new_left = 0
    for line in read(patch).splitlines():
        match = re.match(r"@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@ ?(.*)", line)
        if match:
            # A bare offset (`@@ -1 +1 @@`) means one line.
            old_left = int(match.group(1)) if match.group(1) is not None else 1
            new_left = int(match.group(2)) if match.group(2) is not None else 1
            current = {"header": match.group(3), "post": [], "added": []}
            hunks.append(current)
            continue
        if current is None or line.startswith(("+++", "---", "diff ", "index ")):
            continue
        if not (old_left or new_left):
            continue
        # `\ No newline at end of file` annotates the line before it and is not
        # one of the hunk's lines.
        if line.startswith("\\"):
            continue
        post = current["post"]
        added = current["added"]
        assert isinstance(post, list) and isinstance(added, list)
        if line.startswith("+"):
            post.append(line[1:])
            added.append(line[1:])
            new_left -= 1
        elif line.startswith("-"):
            old_left -= 1
        else:
            post.append(line[1:] if line.startswith(" ") else line)
            old_left -= 1
            new_left -= 1
    return hunks


def hunk_function(header: str) -> str:
    """The function name a hunk header names, if any.

    Exact names matter to the callers: a substring test would let `uninit`
    satisfy a check meant for `init`.
    """
    match = re.search(r"(\w+)\s*\(", header)
    return match.group(1) if match else ""


def extract_anonymous(text: str, origin: Path, member: str, out: list[str]) -> None:
    """An unnamed `enum { MEMBER, ... };` (or struct), anchored by its first member."""
    lines = text.splitlines()
    starts = [
        i
        for i, line in enumerate(lines)
        if re.match(r"^(?:typedef )?(?:struct|enum) \{$", line.strip())
        and any(lines[j].strip().startswith(member) for j in range(i + 1, min(i + 8, len(lines))))
    ]
    if len(starts) != 1:
        fail(f"{origin}: expected exactly one anonymous type starting with {member}, found {len(starts)}")
    start = starts[0]
    close = next((i for i in range(start, len(lines)) if lines[i] in ("};", "} ;")), None)
    if close is None:
        fail(f"{origin}: the anonymous type starting with {member} never closes")
    out.append("\n".join(lines[start : close + 1]))


def extract_symbols(text: str, origin: Path, names: list[str], ret: str | None, out: list[str]) -> None:
    # Without an explicit return type, allow only the characters a C declaration
    # can carry before the name. A permissive `.*` would let the match start
    # inside a comment that happens to mention `name()`, which spans into the
    # real definition and yields a bogus second match.
    #
    # An explicit one is a type, not a pattern: matched literally, and followed
    # by whitespace or directly by the name, because a pointer return type ends
    # in the `*` that touches it (`const char *av_mediacodec_rendered_source(`).
    return_type = re.escape(ret) if ret else r"[A-Za-z_][A-Za-z0-9_ \t*]*?"
    for name in names:
        pattern = re.compile(
            r"^(?:static\s+)?"
            + return_type
            + r"(?:\s+|(?<=\*))"
            + re.escape(name)
            + r"\([^;{]*?\)\n\{.*?^\}",
            re.MULTILINE | re.DOTALL,
        )
        found = pattern.search(text)
        if found is None:
            fail(f"{origin}: no definition of {name}")
        matches = list(pattern.finditer(text))
        # Two definitions in different arms of one `#if`/`#else` chain are
        # alternatives ffmpeg never compiles together -- mediacodec.c defines
        # the rendered-frame ring once under `#if CONFIG_MEDIACODEC` and again
        # as ENOSYS stubs under `#else`. Only definitions that could be
        # compiled together are a redefinition.
        contexts = conditional_contexts(text)
        lines_of = [text.count("\n", 0, match.start()) for match in matches]
        for i in range(len(matches)):
            for j in range(i + 1, len(matches)):
                if not mutually_exclusive(contexts[lines_of[i]], contexts[lines_of[j]]):
                    fail(f"{origin}: {name} is defined more than once")
        out.append(matches[0].group(0))


def emit(path: Path, chunks: list[str], append: bool) -> None:
    body = "\n\n".join(chunks) + "\n"
    if append and path.is_file():
        with path.open("a", encoding="utf-8") as handle:
            handle.write(body)
    else:
        path.write_text(body, encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p, first_help: str, second_help: str):
        p.add_argument("source", type=Path, help=first_help)
        p.add_argument("out", type=Path, help=second_help)
        p.add_argument("--append", action="store_true", help="append instead of overwrite")

    pi = sub.add_parser("post-image", help="write a patch's post-image")
    common(pi, "patch file", "output file")

    rg = sub.add_parser("region", help="write a marked region")
    common(rg, "text or patch file", "output file")
    rg.add_argument("--start", required=True, help="marker text, without the `// --- ` prefix")
    rg.add_argument("--patch", action="store_true", help="read a patch file's post-image")
    rg.add_argument("--added-only", action="store_true", help="keep added lines, drop context")
    rg.add_argument("--require", action="append", default=[], help="text the region must contain")

    hk = sub.add_parser("hunks", help="write a patch's hunks as JSON")
    common(hk, "patch file", "output file")

    rn = sub.add_parser("range", help="write the lines between two landmarks")
    common(rn, "text file", "output file")
    rn.add_argument("--first", required=True)
    rn.add_argument("--last")
    rn.add_argument("--last-inclusive", action="store_true", help="keep the --last line")

    df = sub.add_parser("define", help="write #define lines out of a text file")
    common(df, "text file", "output file")
    df.add_argument("--name", action="append", required=True)

    ty = sub.add_parser("type", help="write struct/enum definitions out of a text file")
    common(ty, "text file", "output file")
    ty.add_argument("--name", action="append", default=[])
    ty.add_argument("--member", action="append", default=[], help="first member of an unnamed type")

    sy = sub.add_parser("symbol", help="write function definitions out of a text file")
    common(sy, "text file", "output file")
    sy.add_argument("--fn", action="append", required=True)
    sy.add_argument("--return", dest="ret", help="exact return type, e.g. 'int64_t' or 'const char *'")

    args = parser.parse_args(argv)

    if args.command == "post-image":
        emit(args.out, ["\n".join(post_image(args.source))], args.append)
        return 0

    if args.command == "region":
        if args.added_only and not args.patch:
            fail("--added-only needs --patch")
        chunk = extract_region(args.source, args.start, args.added_only, args.patch)
        body = "\n".join(chunk)
        for needle in args.require:
            if needle not in body:
                fail(f"{args.source}: the {args.start!r} region does not contain {needle!r}")
        emit(args.out, [body], args.append)
        return 0

    if args.command == "hunks":
        import json

        payload = [
            {
                "func": hunk_function(hunk["header"]),
                "header": hunk["header"],
                "post": hunk["post"],
                "added": hunk["added"],
            }
            for hunk in extract_hunks(args.source)
        ]
        if not payload:
            fail(f"{args.source}: no hunks found")
        text_out = json.dumps(payload, indent=1)
        if args.append and args.out.is_file():
            fail(f"{args.out}: --append does not apply to JSON output")
        args.out.write_text(text_out + "\n", encoding="utf-8")
        return 0

    if args.command == "range":
        emit(
            args.out,
            ["\n".join(extract_range(args.source, args.first, args.last, args.last_inclusive))],
            args.append,
        )
        return 0

    text = read(args.source)
    chunks: list[str] = []
    if args.command == "define":
        extract_defines(text, args.source, args.name, chunks)
    elif args.command == "type":
        if not args.name and not args.member:
            fail("type needs at least one --name or --member")
        extract_types(text, args.source, args.name, chunks)
        for member in args.member:
            extract_anonymous(text, args.source, member, chunks)
    else:
        extract_symbols(text, args.source, args.fn, args.ret, chunks)
    emit(args.out, chunks, args.append)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
