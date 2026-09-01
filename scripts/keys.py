#!/usr/bin/env python3
"""Content-addressed binaries for the libraries this repository builds itself.

Every push to main publishes the artifacts that commit needs, and the committed
manifest on that commit points at them, so any commit can be pinned by a
consumer and gets binaries built from exactly its sources.

The mechanism is a content key per library: the first 12 hex characters of a
sha256 over that library's pinned sources, its patch series, the build flags,
the platform group's canonical platform set, the group's build driver, and the
group's toolchain generation. Assets carry the key in their name, which makes
them immutable -- a name that already exists on the group's rolling prerelease
is the same bytes by construction, so it is never re-uploaded and never
clobbered.

Everything is scoped to a platform group (`--platform-group`, default apple):
each group has its own self-built libraries, canonical platform tuple, build
driver, toolchain file `toolchain/<group>.txt`, rolling release
`binaries-<group>`, and section under `platforms.<group>` in the repo-root
`artifacts.json` manifest. Only the apple group is active today; the code paths
are group-generic so android/linux/windows drivers plug into the same table.

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

Group.json-defined groups -- the android/linux/windows drivers define their
group in `platforms/<group>/group.json` instead of the in-code table (apple
stays in code):

    {
      "assetBase": ".../releases/download/binaries-<group>",
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

There are no gpl=/debug= lines: group.json builds have no flag matrix (the
android build is unconditionally GPL), so --no-gpl and --debug do not move
these keys. The asset one artifact publishes per variant is assetPattern with
{key} and {variant} substituted; the pattern, including its extension, is
authoritative. record-platform renames the unkeyed `<artifact>-<variant><ext>`
(or accepts the already-keyed name), then records

    platforms.<group>.libraries.<artifact> = {
      "key": <key>,
      "prebuilt": {<variant>: {"asset": <name>, "checksum": <sha256 of the
                               asset file, verified by downstream consumers>}}
    }

in artifacts.json; unlike apple there is no merge step, so record-platform is
also the verb that writes the manifest (the publish job collects every
variant into one release dir and runs it once per variant). `verify` and
`stale` demand every variant recorded with the key's asset names; there is no
frameworks concept outside apple (record-frameworks refuses these groups) and
render is a no-op for them.

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
from dataclasses import dataclass
from pathlib import Path

SCHEMA = 2

MANIFEST_PATH = Path("artifacts.json")
VERSIONS_PATH = Path("versions.json")
PATCHES_ROOT = Path("patches")

CHECKSUM_RE = re.compile(r"\A[0-9a-f]{64}\Z")

# Swift Library rawValues whose canonical versions.json component is not just
# the lowercased name.
CANONICAL_OVERRIDES = {"libmpv": "mpv"}


def canonical_component(artifact: str) -> str:
    return CANONICAL_OVERRIDES.get(artifact, artifact.lower())


@dataclass(frozen=True)
class Artifact:
    """One multi-component artifact of a group.json-defined group."""

    # Canonical versions.json component names compiled into every asset.
    components: tuple[str, ...]
    # The ABIs/arches one publish covers, one asset each. Part of the key.
    variants: tuple[str, ...]
    # Asset name template; {key} and {variant} are substituted. The pattern,
    # including its extension, is authoritative.
    asset_pattern: str


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
    # Where the group's rolling prerelease serves its assets.
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
        asset_base="https://github.com/edde746/mpv-build/releases/download/binaries-apple",
        driver_sources=(
            Path("Sources/BuildScripts/Package.swift"),
            Path("Sources/BuildScripts/XCFrameworkBuild/base.swift"),
            Path("Sources/BuildScripts/XCFrameworkBuild/main.swift"),
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
        pattern = spec.get("assetPattern") or ""
        if not components or not variants:
            raise SystemExit(f"{path}: artifact {artifact_id!r} needs components and variants")
        if "{key}" not in pattern or "{variant}" not in pattern:
            raise SystemExit(
                f"{path}: artifact {artifact_id!r} assetPattern must contain "
                "{key} and {variant}"
            )
        artifacts[artifact_id] = Artifact(components, variants, pattern)

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


def resolved_pins(versions: dict, component: str, group: str) -> dict[str, str]:
    """The component's pin fields with the group's overrides folded in."""
    entry = versions.get("components", {}).get(component)
    if entry is None:
        raise SystemExit(f"{VERSIONS_PATH}: no component {component!r}")
    fields = ("version", "url", "ref", "sha256", "commit")
    pins = {field: entry[field] for field in fields if field in entry}
    for field, value in (entry.get("overrides", {}).get(group) or {}).items():
        if field in ("version", "url", "ref", "sha256", "commit"):
            pins[field] = value
    for field in ("version", "url"):
        if field not in pins:
            raise SystemExit(f"{VERSIONS_PATH}: {component} has no {field}")
    return pins


# ---- patch series ---------------------------------------------------------


def _series_entries(path: Path) -> list[str]:
    # A missing series file is an empty series. Order is authoritative.
    if not path.is_file():
        return []
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    return entries


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


def key_inputs(root: Path, group: Group, artifact: str, *, gpl: bool, debug: bool) -> str:
    if group.artifacts is not None:
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
    text += [
        f"gpl={int(gpl)}",
        f"debug={int(debug)}",
        f"platforms={','.join(group.platforms)}",
        f"driver={driver_digest(root, group)}",
        f"toolchain={_sha256_bytes(toolchain_path.read_bytes())}",
    ]
    for name, digest in patch_entries(root, component, group.name):
        text.append(f"patch={name}:{digest}")
    return "\n".join(text) + "\n"


def library_key(root: Path, group: Group, artifact: str, *, gpl: bool, debug: bool) -> str:
    return _sha256_bytes(key_inputs(root, group, artifact, gpl=gpl, debug=debug).encode())[:12]


def all_keys(root: Path, group: Group, *, gpl: bool, debug: bool) -> dict[str, str]:
    return {
        artifact: library_key(root, group, artifact, gpl=gpl, debug=debug)
        for artifact in group.self_built
    }


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
        asset = prebuilt.get(platform)
        if not asset:
            problems.append(f"no prebuilt asset for {platform}")
        elif asset != prebuilt_asset(library, key, platform):
            problems.append(f"prebuilt {platform} asset {asset!r} does not match the key")

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
    return problems


def _artifact_entry_problems(entry: dict, key: str, spec: Artifact) -> list[str]:
    """Why a group.json artifact's manifest entry cannot be trusted for `key`."""
    problems = []
    if entry.get("key") != key:
        problems.append(f"key {entry.get('key')!r} != {key!r}")

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


def _record_variant(args, root: Path, group: Group) -> int:
    """Name one variant's assets by content and record them in the manifest.

    Unlike apple there is no merge step and no frameworks, so record-platform
    is also the verb that writes the manifest: the publish job collects every
    variant into one release dir and runs this once per variant. An entry
    whose key moved is restarted from scratch, so a stale variant can never
    hide under a fresh key.
    """
    release = root / args.release_dir
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)
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
        }
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
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)

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


def cmd_record_frameworks(args, root: Path, group: Group) -> int:
    if group.artifacts is not None:
        raise SystemExit(
            f"the {group.name} group has no frameworks; "
            "record-platform per variant is its only record verb"
        )
    release = root / args.release_dir
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)
    manifest = load_manifest(root)
    section = group_section(manifest, group)

    for library in group.self_built:
        key = keys[library]
        prebuilt = {}
        for platform in group.platforms:
            asset = prebuilt_asset(library, key, platform)
            if (release / asset).is_file():
                prebuilt[platform] = asset

        if not prebuilt:
            # Not compiled in this run: it was restored from its published
            # artifacts, so its existing entry is still the right one.
            print(f"{library}: not rebuilt, keeping the recorded entry")
            continue

        missing = [platform for platform in group.platforms if platform not in prebuilt]
        if missing:
            raise SystemExit(
                f"{library}: rebuilt but missing per-platform artifacts for "
                f"{', '.join(missing)}; refusing to record a partial entry"
            )

        reference = release / prebuilt[group.platforms[0]]
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


def cmd_render(args, root: Path, group: Group) -> int:
    if group.package is None:
        # group.json groups render nothing; a no-op keeps the publish recipe
        # uniform across groups.
        print(f"the {group.name} group renders no package manifest")
        return 0
    manifest = load_manifest(root)
    section = group_section(manifest, group)
    asset_base = section["assetBase"]
    path = root / group.package
    text = path.read_text(encoding="utf-8")
    original = text
    skipped = []

    for library in group.self_built:
        entry = section["libraries"].get(library)
        if not entry:
            continue
        for name, framework in sorted((entry.get("frameworks") or {}).items()):
            url = f"{asset_base}/{framework['asset']}"
            checksum = framework["checksum"]
            match = _target_block(text, name)
            if not match:
                if _has_local_path_target(text, name):
                    skipped.append(name)
                    continue
                raise SystemExit(f"{group.package}: no url/checksum target named {name}")
            replacement = (
                f'{match.group(1)}url: "{url}",{match.group(3)}checksum: "{checksum}"'
            )
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
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)
    if args.show_inputs:
        for library in group.self_built:
            print(f"# {library} -> {keys[library]}")
            print(key_inputs(root, group, library, gpl=args.gpl, debug=args.debug), end="")
        return 0
    print(json.dumps(keys, indent=2, sort_keys=True))
    return 0


def cmd_stale(args, root: Path, group: Group) -> int:
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)
    manifest = load_manifest(root)
    section = group_section(manifest, group)
    for library in group.self_built:
        entry = section["libraries"].get(library)
        if entry_problems(entry, keys[library], library, group):
            print(library)
    return 0


def cmd_verify(args, root: Path, group: Group) -> int:
    keys = all_keys(root, group, gpl=args.gpl, debug=args.debug)
    failures: list[str] = []

    if not (root / MANIFEST_PATH).is_file():
        print(f"FAIL: {MANIFEST_PATH} does not exist, so this commit is not pinnable")
        return 1

    manifest = load_manifest(root)
    section = group_section(manifest, group)
    asset_base = section["assetBase"]
    text = (root / group.package).read_text(encoding="utf-8") if group.package else ""
    allow_local = os.environ.get("MPVKIT_ALLOW_LOCAL_PATH") == "1"

    for library in group.self_built:
        entry = section["libraries"].get(library)
        for problem in entry_problems(entry, keys[library], library, group):
            failures.append(f"{library}: {problem}")
        if not entry or group.package is None:
            continue

        for name, framework in sorted((entry.get("frameworks") or {}).items()):
            expected_url = f"{asset_base}/{framework['asset']}"
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
            if match.group("checksum") != framework["checksum"]:
                failures.append(
                    f"{name}: {group.package} checksum {match.group('checksum')} "
                    f"!= {framework['checksum']}"
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
    # The globals are registered on the main parser and every subparser, with
    # SUPPRESS defaults on the actions, so they work both before and after the
    # verb without the subparser's copy clobbering a value parsed earlier.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--platform-group",
        dest="group",
        choices=sorted(GROUPS),
        default=argparse.SUPPRESS,
        help="platform group to operate on (default: apple)",
    )
    common.add_argument(
        "--no-gpl",
        dest="gpl",
        action="store_false",
        default=argparse.SUPPRESS,
        help="key the non-GPL build (CI builds GPL)",
    )
    common.add_argument(
        "--debug",
        action="store_true",
        default=argparse.SUPPRESS,
        help="key a debug build",
    )

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], parents=[common])
    parser.set_defaults(group="apple", gpl=True, debug=False)
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

    record_platform = subparsers.add_parser(
        "record-platform",
        parents=[common],
        help="name a build leg's thin install trees by content",
    )
    record_platform.add_argument("--platform", required=True)
    record_platform.add_argument("--release-dir", default="dist/release")
    record_platform.set_defaults(func=cmd_record_platform)

    record_frameworks = subparsers.add_parser(
        "record-frameworks",
        parents=[common],
        help="name the merged xcframeworks by content and write the manifest",
    )
    record_frameworks.add_argument("--release-dir", default="dist/release")
    record_frameworks.set_defaults(func=cmd_record_frameworks)

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
    return args.func(args, repo_root(), GROUPS[args.group])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
