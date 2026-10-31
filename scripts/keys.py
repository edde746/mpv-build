#!/usr/bin/env python3
"""Content-addressed binaries for the libraries this repository builds itself.

Every push to main publishes the artifacts that commit needs, and the committed
manifest on that commit points at them, so any commit can be pinned by a
consumer and gets binaries built from exactly its sources.

The mechanism is a content key per library: the first 12 hex characters of a
sha256 over that library's pinned sources, its patch series, the build flags,
the platform group's canonical platform set, the group's build driver, and the
group's toolchain generation. Assets carry the key in their name, so a name
stands for one build's bytes: an asset is never replaced, and never deleted
while a manifest in this repository's history points at it.

Everything is scoped to a platform group (`--platform-group`, default apple):
each group has its own self-built libraries, canonical platform tuple, build
driver, toolchain file `toolchain/<group>.txt`, and section under
`platforms.<group>` in the repo-root `artifacts.json` manifest.

Releases -- every publish run uploads the assets it built to a release of its
own, `binaries-<first 12 hex of the commit built>`: a prerelease holding
nothing else (open-release creates it; release-notes lists its assets once
the run's manifest has landed). A library whose key did not move keeps the
assets an older run built, in that run's release, so the manifest names each
asset's location: wherever an asset name is recorded, an `assetBase` beside
it is the download base it is served from,

    <assetBase>/<asset>

and an asset recorded without one (everything published before per-run
releases) is served from its section's `assetBase`, the group's old rolling
release `binaries-<group>`, which takes no new assets. The record verbs stamp
`--release`'s base on every asset they record, and `verify` accepts a
per-asset assetBase only when it names a per-run release. A run that never
lands its manifest (failed, cancelled or superseded) leaves a release no
manifest points at; the next run's prune-releases deletes every per-run
release that no version of artifacts.json or Package.swift on any ref names.

Key-input text (schema 2) -- the exact bytes each key is derived from, one
line per field, `\\n`-terminated, in this order:

    schema=2
    artifact=<artifact id>           # the library the driver builds, e.g. libmpv
    component=<canonical component>  # its versions.json entry, e.g. mpv
    version=<resolved version>       # pins from versions.json components,
    url=<resolved url>               #   override-aware for the platform group
    ref=<resolved ref>               # each only when the resolved pin
    sha256=<resolved sha256>         #   carries it (git kinds have ref and,
    commit=<resolved commit>         #   once pinned, commit; archives sha256)
    gpl=<0|1>
    debug=<0|1>
    platforms=<comma-joined canonical platform tuple>
    driver=<sha256 over the group's driver sources, path + file sha256 each>
    toolchain=<sha256 of toolchain/<group>.txt>
    patch=<name>:<sha256>            # resolved patch series, one per entry,
                                     #   series order, pool-file sha256

The key is the first 12 hex characters of the sha256 of that text. The patch
series for `<component>` is `patches/<component>/series.common` followed by
`patches/<component>/series.<group>` (blank lines and `#` comments ignored,
order authoritative, a missing series file is empty), each entry naming a file
in `patches/<component>/pool/`.

Two artifact kinds per library:

  <Framework>-<key>.xcframework.zip   what Package.swift links against
  <library>-all-<key>-<platform>.zip  the thin install tree, restored by a
                                      later build instead of recompiling

record-frameworks records an apple library as

    platforms.apple.libraries.<library> = {
      "key": <key>,
      "prebuilt": {<platform>: {"asset": <name>, "assetBase": <base>}},
      "frameworks": {<Framework>: {"asset": <name>, "checksum": <sha256>,
                                   "assetBase": <base>}}
    }

(an entry recorded before per-run releases spells a prebuilt as the bare
asset name and carries no assetBase anywhere).

Group.json-defined groups -- the android/linux/windows drivers define their
group in `platforms/<group>/group.json` instead of the in-code table (apple
stays in code):

    {
      "assetBase": ".../releases/download/binaries-<group>",  # legacy base
      "toolchain": "toolchain/<group>.txt",
      "driver": ["platforms/<group>/build.sh", ...],
      "artifacts": {
        "libmpv-<group>": {
          "components": ["mpv", "ffmpeg", ...],
          "variants": ["arm64-v8a", ...],
          "assetPattern": "libmpv-<group>-{key}-{variant}.tar.gz"
        }
      }
    }

Their artifacts are multi-component: one asset per variant (ABI/arch) carries
every listed component in a single build. Key-input text for a group.json
artifact (schema 2), one line per field, `\n`-terminated, in this order:

    schema=2
    artifact=<artifact id>            # e.g. libmpv-android
    component=<canonical component>   # for every component of the artifact,
    version=<resolved version>        #   in sorted canonical-name order: its
    url=<resolved url>                #   header line then its pin lines from
    ref=<resolved ref>                #   versions.json, override-aware for
    sha256=<resolved sha256>          #   the group, ref/sha256/commit each
    commit=<resolved commit>          #   only when the resolved pin has it
    variants=<comma-joined variants tuple, group.json order>
    driver=<sha256 over the group's driver files, path + file sha256 each>
    toolchain=<sha256 of the group's toolchain file bytes>
    patch=<component>/<name>:<sha256> # every component's resolved patch
                                      #   series, components in the same
                                      #   sorted order, series order within a
                                      #   component, pool-file sha256

There are no gpl=/debug= lines: every build is GPL (there is no LGPL variant to
key) and group.json builds have no flag matrix, so neither flag moves these
keys. The asset one artifact publishes per variant is assetPattern with
{key} and {variant} substituted; the pattern, including its extension, is
authoritative. record-platform renames the unkeyed `<artifact>-<variant><ext>`
(or accepts the already-keyed name), then records

    platforms.<group>.libraries.<artifact> = {
      "key": <key>,
      "prebuilt": {<variant>: {"asset": <name>, "checksum": <sha256 of the
                               asset file, verified by downstream consumers>,
                               "assetBase": <base>}}
    }

in artifacts.json; unlike apple there is no merge step, so record-platform is
also the verb that writes the manifest (the publish job collects every
variant into one release dir and runs it once per variant). `verify` and
`stale` demand every variant recorded with the key's asset names; there is no
frameworks concept outside apple (record-frameworks refuses these groups) and
render is a no-op for them.

Package artifacts -- one asset assembled from other artifacts of the same
group plus sources of its own, such as the mpv-android AAR built from the
libmpv-android tarballs and the JNI glue in platforms/android/lib:

    "mpv-android": {
      "inputs": ["libmpv-android"],         # artifacts of this group it packs
      "sources": ["platforms/android/lib"], # files or directories it builds
      "assetPattern": "mpv-android-{key}.aar"
    }

A package has no components and no variants: its pattern carries {key}
only. Its key-input text (schema 2):

    schema=2
    artifact=<artifact id>
    input=<input artifact id>:<its key>   # every input, sorted
    source=<path>:<sha256>                # every file of the sources, sorted
                                          #   by path; a directory stands for
                                          #   its files git tracks or would
                                          #   track (not ignored)

so an edit to its sources moves its key alone, and anything that moves an
input's key (the group driver and toolchain included) moves it too.
record-packages renames the unkeyed `<artifact><ext>` (the pattern's text
after {key}) and records

    platforms.<group>.libraries.<artifact> = {
      "key": <key>, "asset": <name>, "checksum": <sha256 of the asset file>,
      "assetBase": <base>
    }

`stale`, `verify` and `publish-assets` treat it like any other library.

Debug symbols -- either kind of artifact may name a symbols zip pattern:

    "symbolsPattern": "libmpv-<group>-{key}-{variant}-symbols.zip"

which publishes one zip per variant (a package's variants are its inputs'),
holding the debug files of the binaries that variant ships at the archive
root (scripts/symbols.py packs and checks them). The pattern carries {key}
and {variant}, ends in .zip and differs from assetPattern, unkeyed spelling
included; it is not key input, like assetPattern. The record verbs rename the
unkeyed `<artifact><pattern after {key}>` with the variant substituted (or
accept the keyed name) and add

      "symbols": {<variant>: {"asset": <name>, "checksum": <sha256>, "assetBase": <base>}}

to the entry; an asset whose symbols zip is missing fails the record. `stale`
and `verify` demand every variant's symbols, so an entry recorded before its
artifact named a pattern is stale and the next publish builds and uploads
them, and `publish-assets` uploads them with the other assets.

Deliberately coarse: the key hashes the group's whole build driver, so editing
any of its sources invalidates every self-built library in the group.
Under-invalidation would mean silently shipping stale binaries;
over-invalidation only costs a build. The fast path that matters is preserved:
a commit that only touches patches/mpv/* or mpv's pin in versions.json moves
mpv's key alone.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass, replace
from pathlib import Path

# scripts/patches.py owns the series format; keys.py reads series through it so
# the rule has one implementation.
import patches

SCHEMA = 2

MANIFEST_PATH = Path("artifacts.json")
VERSIONS_PATH = Path("versions.json")
PATCHES_ROOT = Path("patches")

CHECKSUM_RE = re.compile(r"\A[0-9a-f]{64}\Z")

# Where this repository's GitHub releases serve their assets: every group's
# legacy `binaries-<group>` base and every per-run release live under it.
REPOSITORY_URL = "https://github.com/edde746/mpv-build"
DOWNLOAD_BASE = f"{REPOSITORY_URL}/releases/download"
# A publish run's own release: `binaries-` and the first 12 hex characters of
# the commit it built. The legacy group releases never match.
RUN_RELEASE_RE = re.compile(r"\Abinaries-[0-9a-f]{12}\Z")
# A per-run release named anywhere in a manifest or a rendered Package.swift.
RELEASE_REFERENCE_RE = re.compile(r"/releases/download/(binaries-[0-9a-f]{12})(?![0-9a-f])")

# The artifact-name -> versions.json component rule, including its alias table,
# lives in scripts/patches.py: `check` needs it to compare the Swift driver's
# Library names against versions.json, and one rule cannot have two homes.
canonical_component = patches.canonical_component


@dataclass(frozen=True)
class Artifact:
    """One artifact of a group.json-defined group.

    Either a multi-component build (components and variants, one asset per
    variant) or a package (inputs and sources, one asset). Either may also
    publish one debug-symbols zip per variant.
    """

    # Canonical versions.json component names compiled into every asset.
    components: tuple[str, ...]
    # The ABIs/arches one publish covers, one asset each. Part of the key.
    variants: tuple[str, ...]
    # Asset name template; {key} and {variant} are substituted. The pattern,
    # including its extension, is authoritative.
    asset_pattern: str
    # Packages: the artifacts of the same group whose keys feed this key.
    inputs: tuple[str, ...] = ()
    # Packages: the files and directories whose bytes key this artifact.
    sources: tuple[Path, ...] = ()
    # Debug-symbols zip name template ({key} and {variant}); empty when the
    # artifact publishes no symbols. Not part of the key.
    symbols_pattern: str = ""
    # The variants a symbols zip is published for: the artifact's own, or a
    # package's inputs' (the AAR ships one native tree per tarball ABI).
    symbols_variants: tuple[str, ...] = ()

    @property
    def is_package(self) -> bool:
        return bool(self.inputs)


@dataclass(frozen=True)
class Group:
    """One platform group's slice of the build: what it compiles and with what."""

    # Artifact ids of the libraries this group compiles and publishes, in the
    # order they are reported. Everything else the group links is a prebuilt
    # from elsewhere and is left alone.
    self_built: tuple[str, ...]
    # The platforms one publish covers. Part of the key: artifacts for a
    # different platform set are different artifacts.
    platforms: tuple[str, ...]
    # The group's legacy rolling prerelease `binaries-<group>`: its section's
    # assetBase, which serves every asset recorded without an assetBase of its
    # own (all of them published before per-run releases). Nothing new goes there.
    asset_base: str
    # The build driver sources whose bytes key every library in the group.
    driver_sources: tuple[Path, ...]
    # The manifest file the group renders, or None when nothing is rendered.
    package: Path | None
    # group.json-defined groups: the multi-component artifact specs keyed by
    # artifact id. None for the in-code apple group.
    artifacts: dict[str, Artifact] | None = None
    # Explicit toolchain path from group.json; None derives toolchain/<name>.txt.
    toolchain_path: Path | None = None

    @property
    def name(self) -> str:
        return next(name for name, group in GROUPS.items() if group is self)

    @property
    def toolchain(self) -> Path:
        if self.toolchain_path is not None:
            return self.toolchain_path
        return Path("toolchain") / f"{self.name}.txt"


GROUPS = {
    "apple": Group(
        self_built=("libass", "FFmpeg", "libmpv"),
        platforms=("ios", "isimulator", "maccatalyst", "macos", "tvos", "tvsimulator"),
        asset_base=f"{DOWNLOAD_BASE}/binaries-apple",
        driver_sources=(
            Path("Sources/BuildScripts/Package.swift"),
            Path("Sources/BuildScripts/XCFrameworkBuild/base.swift"),
            Path("Sources/BuildScripts/XCFrameworkBuild/main.swift"),
            # The publish job's XCFramework merge writes the bytes of every
            # framework asset, so it keys them like the driver that compiles them.
            Path("scripts/merge_xcframeworks.py"),
        ),
        package=Path("Package.swift"),
    ),
}


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


# ---- group.json groups ----------------------------------------------------


def _group_from_json(path: Path) -> Group:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        raise SystemExit(f"{path}: not valid JSON: {error}")

    def field(key: str):
        value = data.get(key)
        if not value:
            raise SystemExit(f"{path}: missing or empty {key!r}")
        return value

    artifacts = {}
    for artifact_id, spec in sorted(field("artifacts").items()):
        components = tuple(spec.get("components") or ())
        variants = tuple(spec.get("variants") or ())
        inputs = tuple(spec.get("inputs") or ())
        sources = tuple(Path(entry) for entry in spec.get("sources") or ())
        pattern = spec.get("assetPattern") or ""
        if inputs or sources:
            if components or variants or not inputs or not sources:
                raise SystemExit(
                    f"{path}: package artifact {artifact_id!r} needs inputs and sources "
                    "and has no components or variants"
                )
            if "{key}" not in pattern or "{variant}" in pattern:
                raise SystemExit(
                    f"{path}: package artifact {artifact_id!r} assetPattern must contain "
                    "{key} and no {variant}"
                )
        else:
            if not components or not variants:
                raise SystemExit(f"{path}: artifact {artifact_id!r} needs components and variants")
            if "{key}" not in pattern or "{variant}" not in pattern:
                raise SystemExit(
                    f"{path}: artifact {artifact_id!r} assetPattern must contain "
                    "{key} and {variant}"
                )
        symbols = spec.get("symbolsPattern") or ""
        # The unkeyed spellings a build leaves behind (the text after {key}
        # past the artifact id) must differ too, or record would rename one
        # file as both assets.
        unkeyed = pattern.split("{key}", 1)[1] if inputs else "-{variant}" + pattern.rsplit("{variant}", 1)[1]
        if symbols and (
            "{key}" not in symbols
            or "{variant}" not in symbols
            or not symbols.endswith(".zip")
            or symbols == pattern
            or symbols.split("{key}", 1)[1] == unkeyed
        ):
            raise SystemExit(
                f"{path}: artifact {artifact_id!r} symbolsPattern must contain "
                "{key} and {variant}, end in .zip and differ from its assetPattern, "
                "unkeyed spelling included"
            )
        artifacts[artifact_id] = Artifact(
            components, variants, pattern, inputs, sources,
            symbols_pattern=symbols, symbols_variants=variants if symbols else (),
        )

    for artifact_id, spec in list(artifacts.items()):
        for name in spec.inputs:
            if name not in artifacts or artifacts[name].is_package:
                raise SystemExit(
                    f"{path}: package artifact {artifact_id!r} input {name!r} is not a "
                    "component artifact of this group"
                )
        if spec.is_package and spec.symbols_pattern:
            # A package ships what its inputs built, one tree per input
            # variant, so its symbols come per input variant too.
            inherited: list[str] = []
            for name in spec.inputs:
                inherited += [variant for variant in artifacts[name].variants if variant not in inherited]
            artifacts[artifact_id] = replace(spec, symbols_variants=tuple(inherited))

    # The group-wide platform tuple is the ordered union of every artifact's
    # variants; per-artifact checks always use the artifact's own tuple.
    variants: list[str] = []
    for spec in artifacts.values():
        for variant in spec.variants:
            if variant not in variants:
                variants.append(variant)

    return Group(
        self_built=tuple(artifacts),
        platforms=tuple(variants),
        asset_base=field("assetBase"),
        driver_sources=tuple(Path(entry) for entry in field("driver")),
        package=None,
        artifacts=artifacts,
        toolchain_path=Path(field("toolchain")),
    )


def _discover_groups() -> None:
    root = repo_root()
    for path in sorted(root.glob("platforms/*/group.json")):
        name = path.parent.name
        if name in GROUPS:
            raise SystemExit(f"{path}: group {name!r} is already defined in code")
        GROUPS[name] = _group_from_json(path)


_discover_groups()


# ---- versions.json --------------------------------------------------------


def load_versions(root: Path) -> dict:
    path = root / VERSIONS_PATH
    if not path.is_file():
        raise SystemExit(f"{VERSIONS_PATH}: missing; component pins live there now")
    versions = json.loads(path.read_text(encoding="utf-8"))
    if versions.get("formatVersion") != 1:
        raise SystemExit(f"{VERSIONS_PATH}: unsupported formatVersion {versions.get('formatVersion')!r}")
    return versions


def _component_entry(versions: dict, component: str) -> dict:
    entry = versions.get("components", {}).get(component)
    if entry is None:
        raise SystemExit(f"{VERSIONS_PATH}: no component {component!r}")
    return entry


def resolved_kind(versions: dict, component: str, group: str) -> str:
    """The component's acquisition kind with the group's overrides folded in.

    The drivers dispatch on this (`git` clones, everything else fetches a
    file), so it must agree with resolved_pins: a per-platform override that
    switches a prebuilt pin to a git one has to reach both.
    """
    entry = _component_entry(versions, component)
    override = (entry.get("overrides") or {}).get(group) or {}
    kind = override.get("kind", entry.get("kind"))
    if not kind:
        raise SystemExit(f"{VERSIONS_PATH}: {component} has no kind")
    return kind


def resolved_pins(versions: dict, component: str, group: str) -> dict[str, str]:
    """The component's pin fields with the group's overrides folded in."""
    entry = _component_entry(versions, component)
    fields = ("version", "url", "ref", "sha256", "commit")
    pins = {field: entry[field] for field in fields if field in entry}
    for field, value in (entry.get("overrides", {}).get(group) or {}).items():
        if field in ("version", "url", "ref", "sha256", "commit"):
            pins[field] = value
    # An archive pin's identity is its sha256; ref and commit only mean
    # something for a git pin. Folding an override in must not carry the base
    # entry's ref or commit -- describing an unrelated tag and tree -- into the
    # key of a pin that acquires an archive: no tool reads it for an archive,
    # and a bump to the base git pin would move the archive's key for no
    # reason. This is also the invariant platforms/android/download.sh's
    # pin_block relies on when it reads an archive's ref back as empty.
    if resolved_kind(versions, component, group) != "git":
        pins.pop("commit", None)
        pins.pop("ref", None)
    for field in ("version", "url"):
        if field not in pins:
            raise SystemExit(f"{VERSIONS_PATH}: {component} has no {field}")
    return pins


# ---- patch series ---------------------------------------------------------


def _series_entries(path: Path) -> list[str]:
    # The series format lives in scripts/patches.py; this is the same reader so
    # a rule change cannot land in one and miss the other.
    return patches.read_series(path)


def patch_entries(root: Path, component: str, group: str) -> list[tuple[str, str]]:
    """(name, sha256) of every patch in the component's resolved series, in order."""
    directory = root / PATCHES_ROOT / component
    names = _series_entries(directory / "series.common") + _series_entries(
        directory / f"series.{group}"
    )
    entries = []
    for name in names:
        pool_file = directory / "pool" / name
        if not pool_file.is_file():
            raise SystemExit(f"{directory / 'pool' / name}: named by a series file but missing")
        entries.append((name, _sha256_file(pool_file)))
    return entries


# ---- keys -----------------------------------------------------------------


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def driver_digest(root: Path, group: Group) -> str:
    digest = hashlib.sha256()
    for relative in group.driver_sources:
        path = root / relative
        if not path.is_file():
            raise SystemExit(f"missing build driver source: {relative}")
        digest.update(f"{relative}\n".encode())
        digest.update(_sha256_file(path).encode())
        digest.update(b"\n")
    return digest.hexdigest()


def _pin_lines(pins: dict[str, str]) -> list[str]:
    lines = [f"version={pins['version']}", f"url={pins['url']}"]
    for field in ("ref", "sha256", "commit"):
        if field in pins:
            lines.append(f"{field}={pins[field]}")
    return lines


def _toolchain_file(root: Path, group: Group) -> Path:
    toolchain_path = root / group.toolchain
    if not toolchain_path.is_file():
        raise SystemExit(f"{group.toolchain}: missing toolchain generation file")
    return toolchain_path


def _artifact_key_inputs(root: Path, group: Group, artifact: str) -> str:
    """Key-input text for a multi-component group.json artifact.

    No gpl/debug lines: these builds have no flag matrix, so the flags are
    deliberately not part of the key.
    """
    spec = group.artifacts[artifact]
    versions = load_versions(root)
    toolchain_path = _toolchain_file(root, group)

    text = [f"schema={SCHEMA}", f"artifact={artifact}"]
    for component in sorted(spec.components):
        text.append(f"component={component}")
        text.extend(_pin_lines(resolved_pins(versions, component, group.name)))
    text += [
        f"variants={','.join(spec.variants)}",
        f"driver={driver_digest(root, group)}",
        f"toolchain={_sha256_bytes(toolchain_path.read_bytes())}",
    ]
    for component in sorted(spec.components):
        for name, digest in patch_entries(root, component, group.name):
            text.append(f"patch={component}/{name}:{digest}")
    return "\n".join(text) + "\n"


def package_source_files(root: Path, sources: tuple[Path, ...]) -> list[str]:
    """Every file a package's sources name, repository-relative and sorted.

    A file stands for itself; a directory for the files under it that git
    tracks or would track (untracked but not ignored), so build output inside
    it is not source while a new file counts before it is committed.
    """
    files: set[str] = set()
    for source in sources:
        path = root / source
        if path.is_file():
            files.add(source.as_posix())
            continue
        if not path.is_dir():
            raise SystemExit(f"missing package source: {source}")
        listing = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--",
             source.as_posix()],
            cwd=root,
            capture_output=True,
        )
        if listing.returncode != 0:
            raise SystemExit(
                f"git ls-files {source} failed: {listing.stderr.decode().strip()}"
            )
        found = [
            name for name in listing.stdout.decode().split("\0")
            if name and (root / name).is_file()
        ]
        if not found:
            raise SystemExit(f"package source {source} holds no files")
        files.update(found)
    return sorted(files)


def _package_key_inputs(root: Path, group: Group, artifact: str) -> str:
    """Key-input text for a package artifact: its inputs' keys and its sources."""
    spec = group.artifacts[artifact]
    text = [f"schema={SCHEMA}", f"artifact={artifact}"]
    for name in sorted(spec.inputs):
        text.append(f"input={name}:{library_key(root, group, name)}")
    for name in package_source_files(root, spec.sources):
        text.append(f"source={name}:{_sha256_file(root / name)}")
    return "\n".join(text) + "\n"


def key_inputs(root: Path, group: Group, artifact: str) -> str:
    if group.artifacts is not None:
        if group.artifacts[artifact].is_package:
            return _package_key_inputs(root, group, artifact)
        return _artifact_key_inputs(root, group, artifact)

    component = canonical_component(artifact)
    pins = resolved_pins(load_versions(root), component, group.name)
    toolchain_path = _toolchain_file(root, group)

    text = [
        f"schema={SCHEMA}",
        f"artifact={artifact}",
        f"component={component}",
    ]
    text.extend(_pin_lines(pins))
    if group.name == "apple" and artifact == "libmpv":
        # The Swift renderer and Rust core are folded into libmpv.a, and only
        # into it, so their build script keys libmpv rather than the group.
        text.append("component=spatialaudiobridge")
        text.extend(_pin_lines(resolved_pins(load_versions(root), "spatialaudiobridge", group.name)))
        text.append(f"spatial-driver={_sha256_file(root / 'scripts/build_spatial.py')}")
    text += [
        # Every build is GPL: the apple driver hardcodes --enable-gpl/-Dgpl=true
        # and the group.json builds have no flag matrix, so these two lines are
        # constants. They stay because the published apple keys were derived
        # from them; there is no LGPL build and no debug build to key.
        "gpl=1",
        "debug=0",
        f"platforms={','.join(group.platforms)}",
        f"driver={driver_digest(root, group)}",
        f"toolchain={_sha256_bytes(toolchain_path.read_bytes())}",
    ]
    for name, digest in patch_entries(root, component, group.name):
        text.append(f"patch={name}:{digest}")
    if group.name == "apple" and artifact == "libmpv":
        for name, digest in patch_entries(root, "spatialaudiobridge", group.name):
            text.append(f"patch=spatialaudiobridge/{name}:{digest}")
    return "\n".join(text) + "\n"


def library_key(root: Path, group: Group, artifact: str) -> str:
    return _sha256_bytes(key_inputs(root, group, artifact).encode())[:12]


def all_keys(root: Path, group: Group) -> dict[str, str]:
    return {artifact: library_key(root, group, artifact) for artifact in group.self_built}


# ---- manifest -------------------------------------------------------------


def prebuilt_asset(library: str, key: str, platform: str) -> str:
    return f"{library}-all-{key}-{platform}.zip"


def framework_asset(framework: str, key: str) -> str:
    return f"{framework}-{key}.xcframework.zip"


def load_manifest(root: Path) -> dict:
    path = root / MANIFEST_PATH
    if not path.is_file():
        return {"schema": SCHEMA, "platforms": {}}
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA:
        raise SystemExit(f"{MANIFEST_PATH}: unsupported schema {manifest.get('schema')!r}")
    manifest.setdefault("platforms", {})
    return manifest


def group_section(manifest: dict, group: Group) -> dict:
    section = manifest["platforms"].setdefault(group.name, {})
    section.setdefault("assetBase", group.asset_base)
    section.setdefault("libraries", {})
    return section


def save_manifest(root: Path, manifest: dict) -> None:
    path = root / MANIFEST_PATH
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_release_base(release: str | None) -> str:
    """The download base of a publish run's own release; refuses any other name."""
    if not RUN_RELEASE_RE.match(release or ""):
        raise SystemExit(
            f"{release!r} is not a publish run's release (binaries-<first 12 hex of the "
            "commit built>); the legacy binaries-<group> releases take no new assets"
        )
    return f"{DOWNLOAD_BASE}/{release}"


def release_of_base(base: str) -> str:
    """The release tag a download base serves; refuses a base outside this repository."""
    tag = base.removeprefix(DOWNLOAD_BASE + "/")
    if tag == base or not tag or "/" in tag:
        raise SystemExit(f"{base!r} is not a release download base of {REPOSITORY_URL}")
    return tag


def asset_location(section: dict, recorded) -> str:
    """The download base one recorded asset is served from.

    `recorded` is whatever names the asset: its {asset, ...} record (a package
    entry is its own) or, for an apple prebuilt recorded before per-run
    releases, the bare name. Its own assetBase wins; an asset without one is
    served from its section's, the group's legacy release.
    """
    if isinstance(recorded, dict) and recorded.get("assetBase"):
        return recorded["assetBase"]
    return section["assetBase"]


def _location_problems(label: str, recorded) -> list[str]:
    """Why an asset's own assetBase, when it has one, is not a per-run release.

    Only the record verbs write the field, always naming their run's release;
    anything else would point an asset at a legacy group release, which takes
    no new assets, or outside this repository.
    """
    if not isinstance(recorded, dict) or "assetBase" not in recorded:
        return []
    base = recorded["assetBase"]
    prefix = DOWNLOAD_BASE + "/"
    if (
        not isinstance(base, str)
        or not base.startswith(prefix)
        or not RUN_RELEASE_RE.match(base[len(prefix):])
    ):
        return [f"{label}: assetBase {base!r} is not a per-run release of {REPOSITORY_URL}"]
    return []


def entry_problems(entry: dict | None, key: str, library: str, group: Group) -> list[str]:
    """Why this library's manifest entry cannot be trusted for `key`."""
    if not entry:
        return ["no manifest entry"]

    if group.artifacts is not None:
        return _artifact_entry_problems(entry, key, group.artifacts[library])

    problems = []
    if entry.get("key") != key:
        problems.append(f"key {entry.get('key')!r} != {key!r}")

    prebuilt = entry.get("prebuilt") or {}
    for platform in group.platforms:
        recorded = prebuilt.get(platform)
        # Recorded before per-run releases: the bare asset name.
        asset = recorded.get("asset") if isinstance(recorded, dict) else recorded
        if not asset:
            problems.append(f"no prebuilt asset for {platform}")
            continue
        if asset != prebuilt_asset(library, key, platform):
            problems.append(f"prebuilt {platform} asset {asset!r} does not match the key")
        problems += _location_problems(f"prebuilt {platform}", recorded)

    frameworks = entry.get("frameworks") or {}
    if not frameworks:
        problems.append("no frameworks recorded")
    for name, framework in sorted(frameworks.items()):
        asset = framework.get("asset")
        checksum = framework.get("checksum", "")
        if asset != framework_asset(name, key):
            problems.append(f"{name}: asset {asset!r} does not match the key")
        if not CHECKSUM_RE.match(checksum):
            problems.append(f"{name}: checksum {checksum!r} is not a sha256")
        problems += _location_problems(name, framework)
    return problems


def _artifact_entry_problems(entry: dict, key: str, spec: Artifact) -> list[str]:
    """Why a group.json artifact's manifest entry cannot be trusted for `key`."""
    problems = []
    if entry.get("key") != key:
        problems.append(f"key {entry.get('key')!r} != {key!r}")

    if spec.is_package:
        asset = entry.get("asset")
        if asset != spec.asset_pattern.format(key=key):
            problems.append(f"asset {asset!r} does not match the key")
        checksum = entry.get("checksum", "")
        if not CHECKSUM_RE.match(checksum):
            problems.append(f"checksum {checksum!r} is not a sha256")
        problems += _location_problems("asset", entry)
        return problems + _symbols_problems(entry, key, spec)

    prebuilt = entry.get("prebuilt") or {}
    for variant in spec.variants:
        recorded = prebuilt.get(variant)
        if not isinstance(recorded, dict) or not recorded.get("asset"):
            problems.append(f"no prebuilt asset for {variant}")
            continue
        asset = recorded.get("asset")
        if asset != spec.asset_pattern.format(key=key, variant=variant):
            problems.append(f"prebuilt {variant} asset {asset!r} does not match the key")
        checksum = recorded.get("checksum", "")
        if not CHECKSUM_RE.match(checksum):
            problems.append(f"{variant}: checksum {checksum!r} is not a sha256")
        problems += _location_problems(f"prebuilt {variant}", recorded)
    return problems + _symbols_problems(entry, key, spec)


def _symbols_problems(entry: dict, key: str, spec: Artifact) -> list[str]:
    """Why an entry's `symbols` map does not describe `key`'s symbols zips.

    An entry recorded before its artifact published symbols has none, so it
    is stale and the next publish builds and uploads them.
    """
    problems = []
    symbols = entry.get("symbols") or {}
    for variant in spec.symbols_variants:
        recorded = symbols.get(variant)
        if not isinstance(recorded, dict) or not recorded.get("asset"):
            problems.append(f"no symbols asset for {variant}")
            continue
        asset = recorded["asset"]
        if asset != spec.symbols_pattern.format(key=key, variant=variant):
            problems.append(f"symbols {variant} asset {asset!r} does not match the key")
        checksum = recorded.get("checksum", "")
        if not CHECKSUM_RE.match(checksum):
            problems.append(f"symbols {variant}: checksum {checksum!r} is not a sha256")
        problems += _location_problems(f"symbols {variant}", recorded)
    return problems


# ---- recording ------------------------------------------------------------


def swiftpm_checksum(path: Path) -> str:
    """SwiftPM's archive checksum, which is the sha256 of the file.

    Cross-checked against `swift package compute-checksum` when a toolchain is
    on PATH, so a future SwiftPM change to the algorithm cannot slip through.
    """
    digest = _sha256_file(path)
    if shutil.which("swift"):
        try:
            reference = subprocess.run(
                ["swift", "package", "compute-checksum", str(path)],
                capture_output=True,
                text=True,
                timeout=180,
            )
        except (OSError, subprocess.TimeoutExpired):
            return digest
        if reference.returncode == 0:
            reference_digest = reference.stdout.strip()
            if reference_digest and reference_digest != digest:
                raise SystemExit(
                    f"{path.name}: sha256 {digest} disagrees with "
                    f"`swift package compute-checksum` {reference_digest}"
                )
    return digest


def frameworks_in_prebuilt(zip_path: Path) -> list[str]:
    """Framework names a library produced, read out of its thin install tree.

    Mirrors BaseBuild.packageRelease(): a `lib<name>.a` static library becomes
    the `Lib<name>` framework. Reading the artifact rather than the build
    driver keeps this correct when a library's output set changes, because
    BuildFFMPEG.frameworks() itself derives the list from the built libraries.
    """
    names = set()
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            name = Path(info.filename).name
            if not name.endswith(".a") or not name.startswith("lib"):
                continue
            names.add("Lib" + name[len("lib") : -len(".a")])
    return sorted(names)


def _record_symbols(
    release: Path, artifact_id: str, spec: Artifact, key: str, variants: tuple[str, ...], base: str
) -> dict[str, dict[str, str]]:
    """Name the symbols zips of `variants` by content; their manifest entries.

    A build leaves the unkeyed `<artifact><pattern after {key}>` with the
    variant substituted (mpv-android-arm64-v8a-symbols.zip) or the keyed name.
    An asset is recorded with its symbols or not at all: a build that produced
    a binary but not the debug files for it fails here.
    """
    entries = {}
    for variant in variants:
        target = release / spec.symbols_pattern.format(key=key, variant=variant)
        unkeyed = release / (
            artifact_id + spec.symbols_pattern.split("{key}", 1)[1].format(variant=variant)
        )
        if unkeyed.is_file():
            if target.exists():
                target.unlink()
            unkeyed.rename(target)
            print(f"{unkeyed.name} -> {target.name}")
        if not target.is_file():
            raise SystemExit(
                f"{artifact_id}: {variant} was built without its symbols zip {target.name}"
            )
        entries[variant] = {
            "asset": target.name,
            "checksum": _sha256_file(target),
            "assetBase": base,
        }
    return entries


def _record_variant(args, root: Path, group: Group) -> int:
    """Name one variant's assets by content and record them in the manifest.

    Unlike apple there is no merge step and no frameworks, so record-platform
    is also the verb that writes the manifest: the publish job collects every
    variant into one release dir and runs this once per variant. An entry
    whose key moved is restarted from scratch, symbols included, so a stale
    variant can never hide under a fresh key. Every asset recorded here is
    served from `--release`, the run's own release.
    """
    base = run_release_base(args.release)
    release = root / args.release_dir
    keys = all_keys(root, group)
    manifest = load_manifest(root)
    section = group_section(manifest, group)

    recorded = 0
    for artifact_id in group.self_built:
        spec = group.artifacts[artifact_id]
        if args.platform not in spec.variants:
            continue
        key = keys[artifact_id]
        asset = spec.asset_pattern.format(key=key, variant=args.platform)
        # The unkeyed spelling a build leg may leave behind; the extension is
        # whatever the pattern says after {variant}.
        extension = spec.asset_pattern.rsplit("{variant}", 1)[1]
        unkeyed = release / f"{artifact_id}-{args.platform}{extension}"
        target = release / asset
        if unkeyed.is_file():
            if target.exists():
                target.unlink()
            unkeyed.rename(target)
            print(f"{unkeyed.name} -> {target.name}")
        if not target.is_file():
            print(f"{artifact_id}: no {args.platform} artifact (nothing was compiled)")
            continue

        entry = section["libraries"].get(artifact_id)
        if not entry or entry.get("key") != key:
            entry = {"key": key, "prebuilt": {}}
        entry["prebuilt"][args.platform] = {
            "asset": asset,
            "checksum": _sha256_file(target),
            "assetBase": base,
        }
        if spec.symbols_pattern:
            entry.setdefault("symbols", {}).update(
                _record_symbols(release, artifact_id, spec, key, (args.platform,), base)
            )
        section["libraries"][artifact_id] = entry
        recorded += 1
        print(f"{artifact_id}: key {key}, {args.platform} -> {asset}")

    if recorded:
        save_manifest(root, manifest)
        print(f"wrote {MANIFEST_PATH}")
    return 0


def cmd_record_platform(args, root: Path, group: Group) -> int:
    if args.platform not in group.platforms:
        raise SystemExit(
            f"{args.platform!r} is not a {group.name} platform "
            f"(one of: {', '.join(group.platforms)})"
        )
    if group.artifacts is not None:
        return _record_variant(args, root, group)

    release = root / args.release_dir
    keys = all_keys(root, group)

    renamed = 0
    for library in group.self_built:
        source = release / f"{library}-all.zip"
        if not source.is_file():
            continue
        destination = release / prebuilt_asset(library, keys[library], args.platform)
        if destination.exists():
            destination.unlink()
        source.rename(destination)
        print(f"{source.name} -> {destination.name}")
        renamed += 1

    if renamed == 0:
        print("no per-platform artifacts to name (nothing was compiled)")
    return 0


def cmd_record_packages(args, root: Path, group: Group) -> int:
    """Name every package artifact's asset (and symbols zips) by content and record it.

    The build leaves the unkeyed `<artifact><ext>` (or the keyed name) in the
    release dir; a package this run did not build keeps its recorded entry,
    and one it built is served from `--release`, the run's own release.
    """
    base = run_release_base(args.release)
    packages = [
        artifact_id for artifact_id, spec in (group.artifacts or {}).items() if spec.is_package
    ]
    if not packages:
        print(f"the {group.name} group has no package artifacts")
        return 0

    release = root / args.release_dir
    manifest = load_manifest(root)
    section = group_section(manifest, group)
    recorded = 0
    for artifact_id in packages:
        spec = group.artifacts[artifact_id]
        key = library_key(root, group, artifact_id)
        target = release / spec.asset_pattern.format(key=key)
        unkeyed = release / (artifact_id + spec.asset_pattern.split("{key}", 1)[1])
        if unkeyed.is_file():
            if target.exists():
                target.unlink()
            unkeyed.rename(target)
            print(f"{unkeyed.name} -> {target.name}")
        if not target.is_file():
            print(f"{artifact_id}: not built in this run, keeping the recorded entry")
            continue
        entry = {
            "key": key,
            "asset": target.name,
            "checksum": _sha256_file(target),
            "assetBase": base,
        }
        if spec.symbols_pattern:
            entry["symbols"] = _record_symbols(
                release, artifact_id, spec, key, spec.symbols_variants, base
            )
        section["libraries"][artifact_id] = entry
        recorded += 1
        print(f"{artifact_id}: key {key} -> {target.name}")

    if recorded:
        save_manifest(root, manifest)
        print(f"wrote {MANIFEST_PATH}")
    return 0


def cmd_record_frameworks(args, root: Path, group: Group) -> int:
    if group.artifacts is not None:
        raise SystemExit(
            f"the {group.name} group has no frameworks; record-platform per "
            "variant and record-packages are its record verbs"
        )
    base = run_release_base(args.release)
    release = root / args.release_dir
    keys = all_keys(root, group)
    manifest = load_manifest(root)
    section = group_section(manifest, group)

    for library in group.self_built:
        key = keys[library]
        prebuilt = {}
        for platform in group.platforms:
            asset = prebuilt_asset(library, key, platform)
            if (release / asset).is_file():
                prebuilt[platform] = {"asset": asset, "assetBase": base}

        if not prebuilt:
            # Not compiled in this run: it was restored from its published
            # artifacts, so its existing entry, and the older release it
            # names, is still the right one.
            print(f"{library}: not rebuilt, keeping the recorded entry")
            continue

        missing = [platform for platform in group.platforms if platform not in prebuilt]
        if missing:
            raise SystemExit(
                f"{library}: rebuilt but missing per-platform artifacts for "
                f"{', '.join(missing)}; refusing to record a partial entry"
            )

        reference = release / prebuilt[group.platforms[0]]["asset"]
        frameworks = {}
        for name in frameworks_in_prebuilt(reference):
            built = release / f"{name}.xcframework.zip"
            addressed = release / framework_asset(name, key)
            if built.is_file():
                if addressed.exists():
                    addressed.unlink()
                built.rename(addressed)
            elif not addressed.is_file():
                raise SystemExit(f"{library}: {built.name} was not produced by this build")
            frameworks[name] = {
                "asset": addressed.name,
                "checksum": swiftpm_checksum(addressed),
                "assetBase": base,
            }

        if not frameworks:
            raise SystemExit(f"{library}: no static libraries found in {reference.name}")

        section["libraries"][library] = {
            "key": key,
            "prebuilt": prebuilt,
            "frameworks": frameworks,
        }
        print(f"{library}: key {key}, {len(frameworks)} framework(s), {len(prebuilt)} platform(s)")
        for name, framework in sorted(frameworks.items()):
            print(f"  {name} -> {framework['asset']} {framework['checksum'][:12]}")

    save_manifest(root, manifest)
    print(f"wrote {MANIFEST_PATH}")
    return 0


# ---- Package.swift --------------------------------------------------------


def _target_block(text: str, name: str) -> re.Match | None:
    pattern = re.compile(
        r'(name:\s*"' + re.escape(name) + r'",\s*\n\s*)'
        r'url:\s*"(?P<url>[^"]*)",(\s*\n\s*)'
        r'checksum:\s*"(?P<checksum>[^"]*)"'
    )
    return pattern.search(text)


def _has_local_path_target(text: str, name: str) -> bool:
    pattern = re.compile(r'name:\s*"' + re.escape(name) + r'",\s*\n\s*path:\s*"')
    return bool(pattern.search(text))


def framework_expectations(root: Path, group: Group) -> list[tuple[str, str, str]]:
    """(framework, expected url, expected checksum) for every rendered target.

    The one authority for what `render` writes and what `verify` demands: both
    walk this list, so they cannot disagree about a layout the manifest
    describes. Each framework's URL is its own location (asset_location).
    """
    if group.package is None:
        return []
    manifest = load_manifest(root)
    section = group_section(manifest, group)

    expected = []
    for library in group.self_built:
        entry = section["libraries"].get(library)
        if not entry:
            continue
        for name, framework in sorted((entry.get("frameworks") or {}).items()):
            base = asset_location(section, framework)
            expected.append((name, f"{base}/{framework['asset']}", framework["checksum"]))
    return expected


def cmd_render(args, root: Path, group: Group) -> int:
    if group.package is None:
        # group.json groups render nothing; a no-op keeps the publish recipe
        # uniform across groups.
        print(f"the {group.name} group renders no package manifest")
        return 0
    path = root / group.package
    text = path.read_text(encoding="utf-8")
    original = text
    skipped = []

    for name, url, checksum in framework_expectations(root, group):
        match = _target_block(text, name)
        if not match:
            if _has_local_path_target(text, name):
                skipped.append(name)
                continue
            raise SystemExit(f"{group.package}: no url/checksum target named {name}")
        replacement = f'{match.group(1)}url: "{url}",{match.group(3)}checksum: "{checksum}"'
        text = text[: match.start()] + replacement + text[match.end() :]

    if text != original:
        path.write_text(text, encoding="utf-8")
        print(f"rendered {group.package}")
    else:
        print(f"{group.package} already matches the manifest")

    for name in skipped:
        print(f"{name}: pinned to a local path, left untouched")
    return 0


# ---- queries and gate -----------------------------------------------------


def cmd_keys(args, root: Path, group: Group) -> int:
    keys = all_keys(root, group)
    if args.show_inputs:
        for library in group.self_built:
            print(f"# {library} -> {keys[library]}")
            print(key_inputs(root, group, library), end="")
        return 0
    print(json.dumps(keys, indent=2, sort_keys=True))
    return 0


def cmd_groups(args, root: Path, group: Group | None) -> int:
    """Print every group's name, space-separated.

    The workflow matrices and the plan job consume this instead of keeping
    their own copies of the list.
    """
    print(" ".join(sorted(GROUPS)))
    return 0


def cmd_variants(args, root: Path, group: Group) -> int:
    """Print the group's variants (ABIs/arches) in group.json order."""
    for variant in group.platforms:
        print(variant)
    return 0


def cmd_stale(args, root: Path, group: Group) -> int:
    keys = all_keys(root, group)
    manifest = load_manifest(root)
    section = group_section(manifest, group)
    for library in group.self_built:
        entry = section["libraries"].get(library)
        if entry_problems(entry, keys[library], library, group):
            print(library)
    return 0


def _wanted_assets(section: dict) -> list[tuple[str, str, str]]:
    """(library, asset, base) for every asset the manifest pins, deduplicated in order.

    `base` is where the asset is served from (asset_location). Three entry
    shapes reach here: the apple group records `prebuilt` as a platform ->
    {asset, assetBase} map (a bare asset name before per-run releases), a
    group.json group records a variant -> {asset, checksum, assetBase} map,
    and a package records its one `asset` in the entry itself. Either of the
    last two may add a variant -> {asset, checksum, assetBase} `symbols` map.
    """
    wanted = []
    seen = set()
    for library, entry in sorted(section["libraries"].items()):
        records = [entry] if entry.get("asset") else []
        records += [recorded for _, recorded in sorted((entry.get("prebuilt") or {}).items())]
        records += [framework for _, framework in sorted((entry.get("frameworks") or {}).items())]
        records += [symbols for _, symbols in sorted((entry.get("symbols") or {}).items())]
        for recorded in records:
            asset = recorded["asset"] if isinstance(recorded, dict) else recorded
            if asset not in seen:
                seen.add(asset)
                wanted.append((library, asset, asset_location(section, recorded)))
    return wanted


def _published_digests(root: Path, release: str) -> dict[str, str]:
    """Asset name -> remote sha256 for one release of this repository."""
    listing = root / "dist" / f"published-{release}.txt"
    listing.parent.mkdir(parents=True, exist_ok=True)
    with listing.open("w", encoding="utf-8") as handle:
        result = subprocess.run(
            ["gh", "release", "view", release, "--json", "assets",
             "--jq", '.assets[] | "\\(.name) \\(.digest // "")"'],
            stdout=handle,
        )
    if result.returncode != 0:
        raise SystemExit(f"cannot list the assets of release {release}")
    digests = {}
    for line in listing.read_text(encoding="utf-8").splitlines():
        name, _, digest = line.strip().partition(" ")
        if name:
            digests[name] = digest.removeprefix("sha256:")
    return digests


def cmd_publish_assets(args, root: Path, group: Group) -> int:
    """Upload every manifest asset this run built to the run's own release.

    One implementation for every group, so the verify-on-skip rule below
    cannot drift between them. An asset located in `--release` (which
    open-release created) is uploaded there unless an earlier attempt of the
    same run already did; any other asset is served from the older release
    its entry names and must already be there. Nothing is ever uploaded to
    any release but the run's own.
    """
    run_base = run_release_base(args.release)
    if subprocess.run(["gh", "release", "view", args.release], capture_output=True).returncode != 0:
        raise SystemExit(
            f"release {args.release} does not exist; open-release creates it before any upload"
        )

    section = group_section(load_manifest(root), group)
    release_dir = root / args.release_dir
    digests: dict[str, dict[str, str]] = {}

    uploads = []
    missing = []
    for library, asset, base in _wanted_assets(section):
        release = release_of_base(base)
        if release not in digests:
            digests[release] = _published_digests(root, release)
        path = release_dir / asset
        if asset in digests[release]:
            # Verify-on-skip, against the release the manifest names for the
            # asset: a name that is already published must hold exactly the
            # bytes this run has for it, or the checksum recorded in
            # artifacts.json drifts from what consumers download.
            remote = digests[release][asset]
            if path.is_file() and remote:
                local = _sha256_file(path)
                if local != remote:
                    raise SystemExit(
                        f"{asset}: rebuilt bytes {local} != digest {remote} published in "
                        f"{release}; a content-addressed asset must never change bytes -- "
                        "the archive packaging has lost reproducibility"
                    )
                print(f"published {asset} in {release} (bytes verified)")
            else:
                print(f"published {asset} in {release}")
            continue
        if base != run_base:
            missing.append(f"{library}: {asset} (recorded in {release}, which does not hold it)")
        elif path.is_file():
            uploads.append(path)
        else:
            missing.append(f"{library}: {asset} (not built by this run)")

    # A manifest asset that is neither published nor built would leave a dead
    # URL in the rendered package manifest, so refuse to commit that manifest.
    if missing:
        raise SystemExit(
            "manifest references assets that were neither published nor built:\n  "
            + "\n  ".join(missing)
        )

    if not uploads:
        print("every manifest asset is already published")
        return 0
    for path in uploads:
        print(f"upload    {path.name} -> {args.release}")
    # No --clobber: an existing name was verified above, never replaced.
    subprocess.run(
        ["gh", "release", "upload", args.release, *(str(path) for path in uploads)], check=True
    )
    return 0


# ---- per-run releases -----------------------------------------------------


def _check_run_commit(release: str, commit: str) -> None:
    run_release_base(release)
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or release != f"binaries-{commit[:12]}":
        raise SystemExit(f"{release} is not the release of commit {commit!r}")


def _release_notes(commit: str, assets: list[str] | None) -> str:
    lines = [
        f"Binaries the publish workflow built from {REPOSITORY_URL}/commit/{commit}.",
        "",
        "This prerelease holds only the assets that run built. A library whose sources "
        "did not change keeps the assets an older run built, in that run's release: "
        "artifacts.json on the binaries commit that follows this one on main records "
        "where each asset is served from. Asset names are content-addressed, nothing "
        "here is ever replaced, and the release is never deleted once a manifest in the "
        "repository's history points at it.",
        "",
    ]
    if assets is None:
        lines.append("Assets: being uploaded; the run lists them here once its manifest lands.")
    else:
        lines.append("Assets:")
        lines += [f"- `{name}`" for name in sorted(assets)] or ["- none"]
    return "\n".join(lines) + "\n"


def referenced_releases(root: Path) -> set[str]:
    """Every per-run release a manifest of this repository points at.

    Reads artifacts.json and Package.swift at every commit reachable from any
    ref (branches, remote-tracking branches, tags) plus the working tree, so a
    release that any commit names -- and so any consumer pinning a commit --
    is found. A shallow clone cannot see that history and is refused.
    """

    def git(*arguments: str, stdin: str | None = None) -> str:
        return subprocess.run(
            ["git", *arguments], cwd=root, input=stdin, capture_output=True, text=True, check=True
        ).stdout

    if git("rev-parse", "--is-shallow-repository").strip() != "false":
        raise SystemExit(
            "a shallow clone cannot see every manifest in history; fetch it in full "
            "(actions/checkout fetch-depth: 0)"
        )
    paths = (str(MANIFEST_PATH), "Package.swift")
    specs = "".join(
        f"{commit}:{path}\n" for commit in git("rev-list", "--all").split() for path in paths
    )
    blobs = set()
    for line in git("cat-file", "--batch-check=%(objectname) %(objecttype)", stdin=specs).splitlines():
        name, _, kind = line.partition(" ")
        if kind == "blob":
            blobs.add(name)
    text = git("cat-file", "--batch", stdin="".join(f"{blob}\n" for blob in sorted(blobs))) if blobs else ""
    for path in paths:
        if (root / path).is_file():
            text += (root / path).read_text(encoding="utf-8")
    return set(RELEASE_REFERENCE_RE.findall(text))


def cmd_prune_releases(args, root: Path, group: Group | None) -> int:
    """Delete every per-run release no manifest in this repository points at.

    Such a release belongs to a run that never landed its manifest (failed,
    cancelled or superseded), so nothing can download from it. Only
    prereleases named `binaries-<12 hex>` are candidates: the legacy group
    releases and any other release are never touched.
    """
    referenced = referenced_releases(root)
    listing = subprocess.run(
        ["gh", "release", "list", "--limit", "100000", "--json", "tagName,isPrerelease",
         "--jq", '.[] | "\\(.tagName) \\(.isPrerelease)"'],
        capture_output=True, text=True, check=True,
    ).stdout
    kept = 0
    deleted = 0
    for line in listing.splitlines():
        tag, _, prerelease = line.strip().partition(" ")
        if not RUN_RELEASE_RE.match(tag):
            continue
        if tag in referenced or prerelease != "true":
            kept += 1
            continue
        print(f"delete    {tag} (no manifest points at it)")
        subprocess.run(["gh", "release", "delete", tag, "--yes", "--cleanup-tag"], check=True)
        deleted += 1
    print(f"per-run releases: {kept} kept, {deleted} deleted")
    return 0


def cmd_open_release(args, root: Path, group: Group | None) -> int:
    """Create the run's own release, a prerelease of the commit it builds.

    Run after prune-releases, which deletes an earlier attempt's release of
    the same commit unless a manifest points at it. A release that is still
    there is therefore published: its commit already has its binaries, and
    nothing may be added to them.
    """
    _check_run_commit(args.release, args.commit)
    if subprocess.run(["gh", "release", "view", args.release], capture_output=True).returncode == 0:
        raise SystemExit(
            f"{args.release} exists already; prune-releases leaves only releases a manifest "
            "points at, so this commit's binaries are published and nothing may be added"
        )
    subprocess.run(
        ["gh", "release", "create", args.release, "--prerelease", "--target", args.commit,
         "--title", f"Binaries for {args.commit[:12]}",
         "--notes", _release_notes(args.commit, None)],
        check=True,
    )
    print(f"opened {args.release}")
    return 0


def cmd_release_notes(args, root: Path, group: Group | None) -> int:
    """List the source commit and every asset of the run's release in its notes."""
    _check_run_commit(args.release, args.commit)
    names = subprocess.run(
        ["gh", "release", "view", args.release, "--json", "assets", "--jq", ".assets[].name"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    subprocess.run(
        ["gh", "release", "edit", args.release, "--notes", _release_notes(args.commit, names)],
        check=True,
    )
    print(f"{args.release}: notes list {len(names)} asset(s)")
    return 0


def cmd_verify(args, root: Path, group: Group) -> int:
    keys = all_keys(root, group)
    failures: list[str] = []

    if not (root / MANIFEST_PATH).is_file():
        print(f"FAIL: {MANIFEST_PATH} does not exist, so this commit is not pinnable")
        return 1

    manifest = load_manifest(root)
    section = group_section(manifest, group)
    text = (root / group.package).read_text(encoding="utf-8") if group.package else ""
    allow_local = os.environ.get("MPVKIT_ALLOW_LOCAL_PATH") == "1"

    for library in group.self_built:
        entry = section["libraries"].get(library)
        for problem in entry_problems(entry, keys[library], library, group):
            failures.append(f"{library}: {problem}")

    for name, expected_url, expected_checksum in framework_expectations(root, group):
        match = _target_block(text, name)
        if not match:
            if _has_local_path_target(text, name):
                message = (
                    f"{name}: {group.package} pins a local path instead of {expected_url}"
                )
                if allow_local:
                    print(f"warning: {message} (MPVKIT_ALLOW_LOCAL_PATH=1)")
                else:
                    failures.append(message)
            else:
                failures.append(f"{name}: no url/checksum target in {group.package}")
            continue
        if match.group("url") != expected_url:
            failures.append(
                f"{name}: {group.package} url {match.group('url')} != {expected_url}"
            )
        if match.group("checksum") != expected_checksum:
            failures.append(
                f"{name}: {group.package} checksum {match.group('checksum')} "
                f"!= {expected_checksum}"
            )

    if failures:
        print(f"FAIL: the published {group.name} binaries do not describe this commit")
        for failure in failures:
            print(f"  {failure}")
        print("\nRun the publish workflow (or `make build` + record/render) to refresh them.")
        return 1

    for library in group.self_built:
        print(f"{library}: {keys[library]} ok")
    print(f"every self-built {group.name} library is published and pinned for this commit")
    return 0


def main(argv: list[str]) -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--platform-group",
        dest="group",
        choices=sorted(GROUPS),
        default="apple",
        help="platform group to operate on (default: apple)",
    )

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest="command", required=True)

    keys = subparsers.add_parser(
        "keys", parents=[common], help="print the content key of every self-built library"
    )
    keys.add_argument("--show-inputs", action="store_true", help="print what each key hashes")
    keys.set_defaults(func=cmd_keys)

    stale = subparsers.add_parser(
        "stale", parents=[common], help="list libraries whose binaries need building"
    )
    stale.set_defaults(func=cmd_stale)

    # No --platform-group: this verb is what enumerates the groups.
    groups = subparsers.add_parser(
        "groups", help="print every platform group's name, space-separated"
    )
    groups.set_defaults(func=cmd_groups)

    variants = subparsers.add_parser(
        "variants",
        parents=[common],
        help="print the group's variants (ABIs/arches) in group.json order",
    )
    variants.set_defaults(func=cmd_variants)

    run_release_help = "the run's own release, binaries-<first 12 hex of the commit built>"

    publish_assets = subparsers.add_parser(
        "publish-assets",
        parents=[common],
        help="upload the manifest's assets that this run built to the run's own release",
    )
    publish_assets.add_argument("--release-dir", default="dist/release")
    publish_assets.add_argument("--release", required=True, help=run_release_help)
    publish_assets.set_defaults(func=cmd_publish_assets)

    record_platform = subparsers.add_parser(
        "record-platform",
        parents=[common],
        help="name a build leg's thin install trees by content",
    )
    record_platform.add_argument("--platform", required=True)
    record_platform.add_argument("--release-dir", default="dist/release")
    # Only a group.json group records here (apple's legs only rename), so
    # only those need it; _record_variant refuses to run without it.
    record_platform.add_argument("--release", help=run_release_help + " (group.json groups)")
    record_platform.set_defaults(func=cmd_record_platform)

    record_packages = subparsers.add_parser(
        "record-packages",
        parents=[common],
        help="name the group's package artifacts (e.g. the mpv-android AAR) by content",
    )
    record_packages.add_argument("--release-dir", default="dist/release")
    record_packages.add_argument("--release", required=True, help=run_release_help)
    record_packages.set_defaults(func=cmd_record_packages)

    record_frameworks = subparsers.add_parser(
        "record-frameworks",
        parents=[common],
        help="name the merged xcframeworks by content and write the manifest",
    )
    record_frameworks.add_argument("--release-dir", default="dist/release")
    record_frameworks.add_argument("--release", required=True, help=run_release_help)
    record_frameworks.set_defaults(func=cmd_record_frameworks)

    # The release verbs are group-agnostic: one release per run serves every group.
    prune_releases = subparsers.add_parser(
        "prune-releases",
        help="delete every per-run release no manifest in history points at",
    )
    prune_releases.set_defaults(func=cmd_prune_releases)

    for name, func, text in (
        ("open-release", cmd_open_release, "create the run's own prerelease"),
        ("release-notes", cmd_release_notes, "list the run's commit and assets in its release notes"),
    ):
        verb = subparsers.add_parser(name, help=text)
        verb.add_argument("--release", required=True, help=run_release_help)
        verb.add_argument("--commit", required=True, help="the full sha of the commit built")
        verb.set_defaults(func=func)

    render = subparsers.add_parser(
        "render", parents=[common], help="point Package.swift at the manifest's assets"
    )
    render.set_defaults(func=cmd_render)

    verify = subparsers.add_parser(
        "verify",
        parents=[common],
        help="fail unless the manifest and Package.swift describe this commit",
    )
    verify.set_defaults(func=cmd_verify)

    args = parser.parse_args(argv)
    group = GROUPS[args.group] if getattr(args, "group", None) else None
    return args.func(args, repo_root(), group)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
