#!/usr/bin/env bash
# Regression test for scripts/keys.py, which is what makes "pin any commit and
# get the binaries built from it" true.
#
# Most scenarios run against a synthetic repository, so the assertions can be
# exact (which key moved, which asset was renamed, which check failed) without
# a 14-minute build. The real-tree scenarios run only when the unified layout
# (versions.json, toolchain/apple.txt) is present, so the real pins and patch
# series stay covered.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$root" <<'PY'
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

root = Path(sys.argv[1])
tool = root / "scripts" / "keys.py"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def run(repo, *args, expect=0, env=None):
    environment = dict(os.environ)
    if env:
        environment.update(env)
    result = subprocess.run(
        [sys.executable, str(repo / "scripts" / "keys.py"), *args],
        capture_output=True,
        text=True,
        cwd=repo,
        env=environment,
    )
    if expect is not None and result.returncode != expect:
        failures.append(
            f"{' '.join(args)} exited {result.returncode}, expected {expect}\n"
            f"{result.stdout}{result.stderr}"
        )
        print(f"FAIL: {' '.join(args)} exited {result.returncode}, expected {expect}")
        print(result.stdout, result.stderr)
    return result


LIBRARIES = {"libass": ["libass"], "FFmpeg": ["libavcodec", "libavutil"], "libmpv": ["libmpv"]}
COMPONENTS = {"libass": "libass", "FFmpeg": "ffmpeg", "libmpv": "mpv"}
PLATFORMS = ("ios", "isimulator", "maccatalyst", "macos", "tvos", "tvsimulator")
ASSET_BASE = "https://github.com/edde746/mpv-build/releases/download/binaries-apple"
# The publish run the record verbs stamp in these scenarios, and a later one.
RUN_RELEASE = "binaries-0123456789ab"
RUN_BASE = f"https://github.com/edde746/mpv-build/releases/download/{RUN_RELEASE}"
LATER_RELEASE = "binaries-ba9876543210"
LATER_BASE = f"https://github.com/edde746/mpv-build/releases/download/{LATER_RELEASE}"

VERSIONS = {
    "formatVersion": 1,
    "refreshContract": {"rules": []},
    "components": {
        "mpv": {
            "kind": "git",
            "version": "v0.41.0",
            "url": "https://github.com/mpv-player/mpv",
            "ref": "v0.41.0",
            "provenance": "test fixture",
            "platforms": ["apple", "android", "linux", "windows"],
        },
        "ffmpeg": {
            "kind": "git",
            "version": "n8.0.1",
            "url": "https://github.com/FFmpeg/FFmpeg",
            "ref": "n8.0.1",
            "provenance": "test fixture",
            "platforms": ["apple", "android", "linux", "windows"],
        },
        "libass": {
            "kind": "git",
            "version": "0.18.3",
            "url": "https://github.com/edde746/libass",
            "ref": "0.18.3",
            "provenance": "test fixture",
            "platforms": ["apple", "android", "linux", "windows"],
            # An override for another group must not touch apple's key.
            "overrides": {"android": {"version": "0.18.3-ndk"}},
        },
        "spatialaudiobridge": {
            "kind": "git", "version": "1.6.1", "url": "https://example.invalid/spatial",
            "ref": "1" * 40, "commit": "1" * 40, "platforms": ["apple"],
        },
        # A prebuilt dependency keys.py must leave alone.
        "libplacebo": {
            "kind": "prebuilt",
            "version": "7.351.0",
            "url": "https://github.com/mpvkit/libplacebo-build/releases/download/7.351.0/libplacebo-all.zip",
            "sha256": "0" * 64,
            "provenance": "test fixture",
            "platforms": ["apple"],
        },
    },
}

PACKAGE_TEMPLATE = '''// swift-tools-version:5.9
let package = Package(
    name: "MPVKit",
    targets: [
        .binaryTarget(
            name: "Libplacebo",
            url: "https://github.com/mpvkit/libplacebo-build/releases/download/7.351.0/Libplacebo.xcframework.zip",
            checksum: "{placebo}"
        ),
{targets}
        //AUTO_GENERATE_TARGETS_END//
    ]
)
'''

TARGET_TEMPLATE = '''        .binaryTarget(
            name: "{name}",
            url: "https://github.com/edde746/mpv-build/releases/download/1.0.26/{name}.xcframework.zip",
            checksum: "{checksum}"
        ),
'''


def framework_name(static_lib):
    return "Lib" + static_lib[len("lib"):]


def write_versions(repo, versions):
    (repo / "versions.json").write_text(
        json.dumps(versions, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def make_repo(directory):
    """A minimal repository with the unified layout keys.py reads."""
    repo = Path(directory)
    (repo / "scripts").mkdir(parents=True)
    shutil.copy2(tool, repo / "scripts" / "keys.py")
    (repo / "scripts/build_spatial.py").write_text("# source build\n")
    # keys.py reads the series format through patches.py, so the fixture carries
    # it too; the sandbox mirrors the real scripts/ layout.
    shutil.copy2(root / "scripts" / "patches.py", repo / "scripts" / "patches.py")

    write_versions(repo, VERSIONS)

    (repo / "toolchain").mkdir()
    (repo / "toolchain" / "apple.txt").write_text("generation=1\n", encoding="utf-8")

    build_scripts = repo / "Sources" / "BuildScripts"
    (build_scripts / "XCFrameworkBuild").mkdir(parents=True)
    (build_scripts / "Package.swift").write_text("// build scripts package\n", encoding="utf-8")
    (build_scripts / "XCFrameworkBuild" / "base.swift").write_text("// base\n", encoding="utf-8")
    (build_scripts / "XCFrameworkBuild" / "main.swift").write_text("// main\n", encoding="utf-8")
    (repo / "scripts" / "merge_xcframeworks.py").write_text("# merge\n", encoding="utf-8")

    # mpv and ffmpeg carry patches; libass deliberately has no patches/ dir at
    # all in this fixture, so a component without one stays covered.
    for component in ("mpv", "ffmpeg"):
        pool = repo / "patches" / component / "pool"
        pool.mkdir(parents=True)
        (pool / "0001-first.patch").write_text(f"--- {component}\n", encoding="utf-8")
        (pool / "0002-second.patch").write_text(f"--- {component} again\n", encoding="utf-8")
        (repo / "patches" / component / "series.common").write_text(
            "# applied everywhere\n0001-first.patch\n\n0002-second.patch\n", encoding="utf-8"
        )

    targets = "".join(
        TARGET_TEMPLATE.format(name=framework_name(static), checksum="0" * 64)
        for statics in LIBRARIES.values()
        for static in statics
    )
    (repo / "Package.swift").write_text(
        PACKAGE_TEMPLATE.format(placebo="1" * 64, targets=targets), encoding="utf-8"
    )
    return repo


def build_artifacts(repo, libraries=LIBRARIES, platforms=PLATFORMS):
    """What a `make build` leg leaves in dist/release before record-platform."""
    release = repo / "dist" / "release"
    release.mkdir(parents=True, exist_ok=True)
    for library, statics in libraries.items():
        all_zip = release / f"{library}-all.zip"
        with zipfile.ZipFile(all_zip, "w") as archive:
            for platform in platforms:
                for static in statics:
                    archive.writestr(
                        f"lib/{platform}/thin/arm64/lib/{static}.a", f"{library}-{platform}"
                    )
            archive.writestr("include/header.h", "// header")
        for static in statics:
            name = framework_name(static)
            with zipfile.ZipFile(release / f"{name}.xcframework.zip", "w") as archive:
                archive.writestr(f"{name}.xcframework/Info.plist", f"{library}:{name}")
    return release


def publish(repo, libraries=LIBRARIES, platforms=PLATFORMS, release=RUN_RELEASE):
    """One full pass: build every leg, name artifacts, record, render."""
    for platform in platforms:
        build_artifacts(repo, libraries=libraries, platforms=[platform])
        run(repo, "record-platform", "--platform", platform, "--release-dir", "dist/release")
    run(repo, "record-frameworks", "--release-dir", "dist/release", "--release", release)
    run(repo, "render")


def keys(repo, *extra):
    return json.loads(run(repo, "keys", *extra).stdout)


def manifest_of(repo):
    return json.loads((repo / "artifacts.json").read_text())


def apple_libraries(repo):
    return manifest_of(repo)["platforms"]["apple"]["libraries"]


# 1. The real repository: the real pins, patch series and toolchain, once the
#    unified layout has landed.
if (root / "versions.json").is_file() and (root / "toolchain" / "apple.txt").is_file():
    real = keys(root)
    check(sorted(real) == ["FFmpeg", "libass", "libmpv"], f"real repo keys: {sorted(real)}")
    check(
        all(
            len(value) == 12 and all(c in "0123456789abcdef" for c in value)
            for value in real.values()
        ),
        f"real repo keys are not 12 hex chars: {real}",
    )
    check(keys(root) == real, "keys must be reproducible for the same tree")
    inputs = run(root, "keys", "--show-inputs").stdout
    check("schema=2" in inputs, "key inputs carry the schema")
    check("component=mpv" in inputs, "libmpv's pins come from the canonical mpv component")
    print(f"real repo: {json.dumps(real, sort_keys=True)}")
else:
    print("real repo: skipped (versions.json / toolchain/apple.txt not landed yet)")

# The source compiled into libmpv is part of its key, independently of mpv's own pin.
with tempfile.TemporaryDirectory() as tmp:
    repo = Path(tmp)
    make_repo(repo)
    before = keys(repo)
    path = repo / "versions.json"
    pins = json.loads(path.read_text())
    pins["components"]["spatialaudiobridge"]["commit"] = "2" * 40
    path.write_text(json.dumps(pins))
    after = keys(repo)
    check(before["libmpv"] != after["libmpv"], "SpatialAudioBridge source moves libmpv's key")
    check(before["FFmpeg"] == after["FFmpeg"], "SpatialAudioBridge source does not move FFmpeg")

# The script that builds it is too, and keys nothing else.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    before = keys(repo)
    (repo / "scripts/build_spatial.py").write_text("# changed source build\n")
    after = keys(repo)
    check(before["libmpv"] != after["libmpv"], "the spatial build script moves libmpv's key")
    check(before["FFmpeg"] == after["FFmpeg"] and before["libass"] == after["libass"],
          "the spatial build script does not move FFmpeg or libass")

# A renderer integration patch is another input to libmpv, but not FFmpeg.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    before = keys(repo)
    pool = repo / "patches/spatialaudiobridge/pool"
    pool.mkdir(parents=True)
    patch = pool / "progress.patch"
    patch.write_text("first renderer patch\n")
    check(keys(repo) == before, "unreferenced SAB patches do not affect keys")
    (pool.parent / "series.apple").write_text("progress.patch\n")
    added = keys(repo)
    check(added["libmpv"] != before["libmpv"], "SAB patch series moves libmpv")
    check(added["FFmpeg"] == before["FFmpeg"], "SAB patches do not move FFmpeg")
    patch.write_text("changed renderer patch\n")
    check(keys(repo)["libmpv"] != added["libmpv"], "SAB patch bytes move libmpv")

# 2. The committed manifest describes the published apple assets.
committed = root / "artifacts.json"
if committed.is_file():
    manifest = json.loads(committed.read_text(encoding="utf-8"))
    check(manifest.get("schema") == 2, "committed artifacts.json is schema 2")
    apple = manifest.get("platforms", {}).get("apple", {})
    check(apple.get("assetBase") == ASSET_BASE, f"apple assetBase: {apple.get('assetBase')!r}")
    print(f"committed manifest: {len(apple.get('libraries', {}))} apple libraries")
else:
    print("committed manifest: skipped (artifacts.json not committed yet)")

# 3. A patch edit moves exactly that library's key; order and bytes both count.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    before = keys(repo)

    # An unreferenced pool file must not move any key: only the series counts.
    stray = repo / "patches/mpv/pool/9999-unreferenced.patch"
    stray.write_text("--- stray\n", encoding="utf-8")
    check(keys(repo) == before, "a pool file no series names must not move a key")
    stray.unlink()

    patch = repo / "patches/mpv/pool/0024-new.patch"
    patch.write_text("--- a/video/out/vo_avfoundation.m\n", encoding="utf-8")
    series = repo / "patches/mpv/series.apple"
    series.write_text("0024-new.patch\n", encoding="utf-8")
    after = keys(repo)
    check(before["libmpv"] != after["libmpv"], "a new mpv patch must move libmpv's key")
    check(before["FFmpeg"] == after["FFmpeg"], "an mpv patch must not move FFmpeg's key")
    check(before["libass"] == after["libass"], "an mpv patch must not move libass's key")
    patch.write_text("--- a/video/out/vo_avfoundation.m\n+ edited\n", encoding="utf-8")
    check(keys(repo)["libmpv"] != after["libmpv"], "editing a patch's bytes must move the key")

    # Same patch set, different order: a different build, so a different key.
    ordered = keys(repo)
    common = repo / "patches/mpv/series.common"
    common.write_text("0002-second.patch\n0001-first.patch\n", encoding="utf-8")
    swapped = keys(repo)
    check(
        ordered["libmpv"] != swapped["libmpv"],
        "reordering a series must move the key even with the same patch set",
    )
    check(ordered["FFmpeg"] == swapped["FFmpeg"], "reordering mpv's series only moves mpv")

    # A series naming a missing pool file is a hard error, not a silent skip.
    common.write_text("0001-first.patch\n0404-missing.patch\n", encoding="utf-8")
    result = run(repo, "keys", expect=1)
    check("0404-missing.patch" in result.stderr, "a dangling series entry is reported")
    print("patches: series order, bytes and membership all key; strays do not")

# 4. Pins come from versions.json, override-aware for the platform group.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    before = keys(repo)

    # Another group's override is not apple's business.
    versions = json.loads(json.dumps(VERSIONS))
    versions["components"]["libass"]["overrides"]["android"]["version"] = "changed"
    write_versions(repo, versions)
    check(keys(repo) == before, "an android override must not move apple keys")

    # An apple override moves exactly the overridden component's key.
    versions = json.loads(json.dumps(VERSIONS))
    versions["components"]["libass"]["overrides"]["apple"] = {
        "version": "0.18.4",
        "url": "https://github.com/edde746/libass-apple",
    }
    write_versions(repo, versions)
    overridden = keys(repo)
    check(before["libass"] != overridden["libass"], "an apple override must move libass's key")
    check(before["libmpv"] == overridden["libmpv"], "an apple libass override only moves libass")

    # A base pin bump moves exactly that component's key.
    versions = json.loads(json.dumps(VERSIONS))
    versions["components"]["mpv"]["version"] = "v0.42.0"
    versions["components"]["mpv"]["ref"] = "v0.42.0"
    write_versions(repo, versions)
    bumped = keys(repo)
    check(before["libmpv"] != bumped["libmpv"], "an mpv pin bump must move libmpv's key")
    check(before["libass"] == bumped["libass"], "an mpv pin bump must not move libass's key")

    # An archive override's key is the bytes it fetches. `resolved_pins` folds
    # the override over the base entry, so before 9b6c68d the base git pin's
    # ref -- and commit -- leaked into it: bumping the base pin, which names a
    # different tree entirely, moved the archive's key for no reason. The same
    # is true of the fold's other direction, so the sha256 must still move it.
    def archive_override(base_ref, base_commit, sha256):
        versions = json.loads(json.dumps(VERSIONS))
        versions["components"]["mpv"]["ref"] = base_ref
        versions["components"]["mpv"]["commit"] = base_commit
        versions["components"]["mpv"]["overrides"] = {
            "apple": {
                "kind": "archive",
                "version": "0.41.0",
                "url": "https://github.com/mpv-player/mpv/archive/refs/tags/v0.41.0.tar.gz",
                "sha256": sha256,
            }
        }
        write_versions(repo, versions)
        return keys(repo)

    archived = archive_override("v0.41.0", "1" * 40, "a" * 64)
    rebased = archive_override("v0.42.0", "2" * 40, "a" * 64)
    check(
        archived["libmpv"] == rebased["libmpv"],
        "an archive override's key must not inherit the base git pin's ref or commit",
    )
    repinned = archive_override("v0.41.0", "1" * 40, "b" * 64)
    check(
        archived["libmpv"] != repinned["libmpv"],
        "an archive override's key must move with the sha256 it actually fetches",
    )

    # A component keys.py needs must exist.
    versions = json.loads(json.dumps(VERSIONS))
    del versions["components"]["mpv"]
    write_versions(repo, versions)
    result = run(repo, "keys", expect=1)
    check("mpv" in result.stderr, "a missing component is reported")
    print("pins: versions.json is authoritative and override-aware per group")

# 5. Build-driver and toolchain changes invalidate everything, by design.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    before = keys(repo)
    base = repo / "Sources/BuildScripts/XCFrameworkBuild/base.swift"
    base.write_text(base.read_text() + "// tweak\n", encoding="utf-8")
    after = keys(repo)
    check(
        all(after[library] != before[library] for library in before),
        "a build-driver edit must invalidate every self-built library",
    )
    toolchain = repo / "toolchain" / "apple.txt"
    toolchain.write_text("generation=2\n", encoding="utf-8")
    bumped = keys(repo)
    check(
        all(bumped[library] != after[library] for library in after),
        "a toolchain generation bump must invalidate every self-built library",
    )
    print("build-driver and toolchain changes invalidate all three libraries")

# 6. The group flag selects the group; unknown groups do not.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    plain = keys(repo)
    check(keys(repo, "--platform-group", "apple") == plain, "explicit apple selects the apple group")
    run(repo, "keys", "--platform-group", "haiku", expect=2)
    print("--platform-group: selects the group, unknown groups rejected")

# 7. A full publish pass, then the steady state: nothing stale, verify passes.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    stale = run(repo, "stale", "--platform-group", "apple").stdout.split()
    check(sorted(stale) == ["FFmpeg", "libass", "libmpv"], f"a fresh repo is all stale: {stale}")
    run(repo, "verify", expect=1)

    publish(repo)
    check(run(repo, "stale").stdout.strip() == "", "nothing may be stale after publishing")
    run(repo, "verify")

    manifest = manifest_of(repo)
    check(manifest["schema"] == 2, "the manifest is schema 2")
    check(sorted(manifest["platforms"]) == ["apple"], "only the apple section is written")
    check(
        manifest["platforms"]["apple"]["assetBase"] == ASSET_BASE,
        f"assetBase: {manifest['platforms']['apple']['assetBase']}",
    )
    expected_key = keys(repo)["FFmpeg"]
    entry = apple_libraries(repo)["FFmpeg"]
    check(entry["key"] == expected_key, "the manifest records the computed key")
    check(
        sorted(entry["frameworks"]) == ["Libavcodec", "Libavutil"],
        f"FFmpeg's frameworks come from its static libraries: {sorted(entry['frameworks'])}",
    )
    check(
        sorted(entry["prebuilt"]) == sorted(PLATFORMS),
        f"every platform is recorded: {sorted(entry['prebuilt'])}",
    )
    check(
        entry["prebuilt"]["tvos"] == {"asset": f"FFmpeg-all-{expected_key}-tvos.zip", "assetBase": RUN_BASE},
        f"prebuilt asset and its run's release: {entry['prebuilt']['tvos']}",
    )
    check(
        entry["frameworks"]["Libavcodec"]["assetBase"] == RUN_BASE,
        "a framework is recorded in its run's release",
    )
    asset = entry["frameworks"]["Libavcodec"]["asset"]
    check(asset == f"Libavcodec-{expected_key}.xcframework.zip", f"framework asset name: {asset}")
    check((repo / "dist/release" / asset).is_file(), f"{asset} must exist to be uploaded")

    digest = hashlib.sha256((repo / "dist/release" / asset).read_bytes()).hexdigest()
    check(
        entry["frameworks"]["Libavcodec"]["checksum"] == digest,
        "the recorded checksum is the sha256 of the archive SwiftPM will fetch",
    )

    package = (repo / "Package.swift").read_text()
    check(
        f'url: "{RUN_BASE}/{asset}"' in package,
        "Package.swift points at the content-addressed asset in its run's release",
    )
    check(digest in package, "Package.swift carries the recorded checksum")
    check(
        "libplacebo-build/releases/download/7.351.0/Libplacebo.xcframework.zip" in package,
        "prebuilt dependencies are left alone",
    )
    check("1.0.26" not in package, "no release-version URL survives a render")

    rendered_again = run(repo, "render")
    check("already matches" in rendered_again.stdout, "render is idempotent")
    print("publish pass: manifest, asset names, checksums and Package.swift agree")

# 8. Every way the gate must fail.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    publish(repo)
    run(repo, "verify")
    package_path = repo / "Package.swift"
    good_package = package_path.read_text()
    good_manifest = (repo / "artifacts.json").read_text()

    # Mutate a self-built framework's checksum, not the first one in the file:
    # that belongs to a prebuilt dependency the gate deliberately ignores.
    recorded = apple_libraries(repo)["libmpv"]["frameworks"]["Libmpv"]["checksum"]
    mutated = ("0" if recorded[0] != "0" else "1") + recorded[1:]
    package_path.write_text(good_package.replace(recorded, mutated), encoding="utf-8")
    result = run(repo, "verify", expect=1)
    check("checksum" in result.stdout, "a tampered Package.swift checksum is reported")
    package_path.write_text(good_package, encoding="utf-8")

    package_path.write_text(
        good_package.replace(f"/{RUN_RELEASE}/Libmpv-", f"/{RUN_RELEASE}/Libmpv-stale-"),
        encoding="utf-8",
    )
    result = run(repo, "verify", expect=1)
    check("url" in result.stdout, "a tampered Package.swift url is reported")
    package_path.write_text(good_package, encoding="utf-8")

    # Sources moved on after the binaries were published: exactly the state a
    # commit must never be pinned in.
    later = repo / "patches/mpv/pool/0025-later.patch"
    later.write_text("x\n", encoding="utf-8")
    series = repo / "patches/mpv/series.apple"
    series.write_text("0025-later.patch\n", encoding="utf-8")
    result = run(repo, "verify", expect=1)
    check("libmpv" in result.stdout, "a source change after publishing fails the gate")
    check(run(repo, "stale").stdout.split() == ["libmpv"], "and marks only libmpv stale")
    later.unlink()
    series.unlink()
    run(repo, "verify")

    # A developer's local path override must never be committed, but must not
    # get in the way of a local build either.
    libmpv = apple_libraries(repo)["libmpv"]["frameworks"]["Libmpv"]
    package_path.write_text(
        good_package.replace(
            f'url: "{RUN_BASE}/{libmpv["asset"]}",\n'
            f'            checksum: "{libmpv["checksum"]}"',
            'path: "dist/release/Libmpv.xcframework.zip"',
        ),
        encoding="utf-8",
    )
    result = run(repo, "verify", expect=1)
    check("local path" in result.stdout, "a local path override fails the gate")
    result = run(repo, "verify", env={"MPVKIT_ALLOW_LOCAL_PATH": "1"})
    check("warning" in result.stdout, "and is a warning when explicitly allowed")
    result = run(repo, "render")
    check("left untouched" in result.stdout, "render leaves a local override alone")
    package_path.write_text(good_package, encoding="utf-8")
    check(
        (repo / "artifacts.json").read_text() == good_manifest,
        "none of the failure paths mutated the manifest",
    )
    print("gate: tampering, stale sources and local overrides all fail verify")

# 9. A build leg that did not produce every platform must not be recorded.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    for platform in ("ios", "macos"):
        build_artifacts(repo, platforms=[platform])
        run(repo, "record-platform", "--platform", platform, "--release-dir", "dist/release")
    result = run(
        repo, "record-frameworks", "--release-dir", "dist/release", "--release", RUN_RELEASE, expect=1
    )
    check(
        "refusing to record a partial entry" in (result.stdout + result.stderr),
        "a partial platform set is refused",
    )
    check(not (repo / "artifacts.json").exists(), "and nothing is written")
    run(repo, "record-platform", "--platform", "wasm", "--release-dir", "dist/release", expect=1)
    print("partial build: refused, manifest untouched; unknown platforms rejected")

# 10. Only the rebuilt libraries are re-recorded; the others keep their entries.
#     Other groups' manifest sections survive untouched.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    publish(repo)
    manifest = manifest_of(repo)
    manifest["platforms"]["android"] = {"assetBase": "https://example.invalid", "libraries": {}}
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    before = apple_libraries(repo)
    later = repo / "patches/mpv/pool/0025-later.patch"
    later.write_text("x\n", encoding="utf-8")
    (repo / "patches/mpv/series.apple").write_text("0025-later.patch\n", encoding="utf-8")
    check(run(repo, "stale").stdout.split() == ["libmpv"], "only libmpv is stale")
    # A later run starts from an empty release dir.
    shutil.rmtree(repo / "dist")
    publish(repo, libraries={"libmpv": ["libmpv"]}, release=LATER_RELEASE)
    after = apple_libraries(repo)
    check(
        after["FFmpeg"] == before["FFmpeg"],
        "an untouched library's entry survives a partial publish, its older release included",
    )
    check(after["libmpv"] != before["libmpv"], "the rebuilt library's entry is replaced")
    check(
        after["libmpv"]["frameworks"]["Libmpv"]["assetBase"] == LATER_BASE
        and after["FFmpeg"]["frameworks"]["Libavcodec"]["assetBase"] == RUN_BASE,
        "the rebuilt library is in the later run's release, the untouched one in the earlier's",
    )
    package = (repo / "Package.swift").read_text()
    check(
        f'{LATER_BASE}/{after["libmpv"]["frameworks"]["Libmpv"]["asset"]}' in package
        and f'{RUN_BASE}/{after["FFmpeg"]["frameworks"]["Libavcodec"]["asset"]}' in package,
        "Package.swift renders each framework from its own release",
    )
    check(
        manifest_of(repo)["platforms"]["android"]
        == {"assetBase": "https://example.invalid", "libraries": {}},
        "another group's section survives an apple publish verbatim",
    )
    run(repo, "verify")
    print("incremental publish: only the stale library's entry changes")

# 10b. An apple entry recorded before per-run releases (prebuilt as bare names,
#      no assetBase anywhere) keeps resolving to the group's legacy release,
#      beside an entry a per-run publish recorded in its own release.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    publish(repo)
    manifest = manifest_of(repo)
    for entry in manifest["platforms"]["apple"]["libraries"].values():
        entry["prebuilt"] = {platform: recorded["asset"] for platform, recorded in entry["prebuilt"].items()}
        for framework in entry["frameworks"].values():
            del framework["assetBase"]
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    run(repo, "render")
    check(run(repo, "stale").stdout.strip() == "", "an entry without assetBase is still published")
    run(repo, "verify")
    legacy = apple_libraries(repo)
    libavcodec = legacy["FFmpeg"]["frameworks"]["Libavcodec"]["asset"]
    check(
        f'url: "{ASSET_BASE}/{libavcodec}"' in (repo / "Package.swift").read_text(),
        "an old framework entry renders from the group's legacy release",
    )

    later = repo / "patches/mpv/pool/0025-later.patch"
    later.write_text("x\n", encoding="utf-8")
    (repo / "patches/mpv/series.apple").write_text("0025-later.patch\n", encoding="utf-8")
    shutil.rmtree(repo / "dist")
    publish(repo, libraries={"libmpv": ["libmpv"]}, release=LATER_RELEASE)
    after = apple_libraries(repo)
    check(after["FFmpeg"] == legacy["FFmpeg"], "the old entry is left exactly as it was")
    check(
        after["libmpv"]["prebuilt"]["ios"]["assetBase"] == LATER_BASE,
        "the rebuilt library's thin zips are in the later run's release",
    )
    package = (repo / "Package.swift").read_text()
    check(
        f'url: "{ASSET_BASE}/{libavcodec}"' in package
        and f'url: "{LATER_BASE}/{after["libmpv"]["frameworks"]["Libmpv"]["asset"]}"' in package,
        "Package.swift mixes the legacy release and the later run's, one URL per framework",
    )
    run(repo, "verify")
    print("locations: an old entry resolves to the group base, a new one to its run's release")

# 11. group.json-defined groups: platforms/<group>/group.json defines the
#     group; keys are multi-component; recording is per variant with
#     checksums; there are no frameworks and nothing to render.
GROUP_JSON = {
    "assetBase": "https://github.com/edde746/mpv-build/releases/download/binaries-android",
    "toolchain": "toolchain/android.txt",
    "driver": ["platforms/android/build.sh", "platforms/android/package.sh"],
    "artifacts": {
        "libmpv-android": {
            "components": ["mpv", "ffmpeg", "libass"],
            "variants": ["arm64-v8a", "x86_64"],
            "assetPattern": "libmpv-android-{key}-{variant}.tar.gz",
        }
    },
}


def write_group_json(repo, data):
    (repo / "platforms" / "android" / "group.json").write_text(
        json.dumps(data, indent=2) + "\n", encoding="utf-8"
    )


def make_android_repo(directory):
    repo = make_repo(directory)
    platform = repo / "platforms" / "android"
    platform.mkdir(parents=True)
    (platform / "build.sh").write_text("#!/bin/bash\nbuild\n", encoding="utf-8")
    (platform / "package.sh").write_text("#!/bin/bash\npackage\n", encoding="utf-8")
    write_group_json(repo, GROUP_JSON)
    (repo / "toolchain" / "android.txt").write_text(
        "ndk=29.0.14206865\ngeneration=1\n", encoding="utf-8"
    )
    return repo


def android_key(repo):
    return keys(repo, "--platform-group", "android")["libmpv-android"]


with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    apple_before = keys(repo)
    # Bolt the android group onto the same tree: apple must not notice.
    platform = repo / "platforms" / "android"
    platform.mkdir(parents=True)
    (platform / "build.sh").write_text("#!/bin/bash\nbuild\n", encoding="utf-8")
    (platform / "package.sh").write_text("#!/bin/bash\npackage\n", encoding="utf-8")
    write_group_json(repo, GROUP_JSON)
    (repo / "toolchain" / "android.txt").write_text(
        "ndk=29.0.14206865\ngeneration=1\n", encoding="utf-8"
    )
    check(keys(repo) == apple_before, "discovering a group.json group must not move apple keys")

    android = keys(repo, "--platform-group", "android")
    check(sorted(android) == ["libmpv-android"], f"android keys: {sorted(android)}")
    key = android["libmpv-android"]
    check(
        len(key) == 12 and all(c in "0123456789abcdef" for c in key),
        f"android key is not 12 hex chars: {key}",
    )
    check(android_key(repo) == key, "android keys must be reproducible for the same tree")

    inputs = run(repo, "keys", "--platform-group", "android", "--show-inputs").stdout
    check("artifact=libmpv-android" in inputs, "android inputs name the artifact")
    check("gpl=" not in inputs, "android inputs carry no gpl/debug lines")
    check("variants=arm64-v8a,x86_64" in inputs, "android inputs carry the variants tuple")
    check(
        inputs.index("component=ffmpeg") < inputs.index("component=libass") < inputs.index("component=mpv"),
        "components appear in sorted canonical order",
    )
    check("patch=mpv/0001-first.patch:" in inputs, "patch lines are component-prefixed")
    check(
        inputs.index("patch=ffmpeg/0001-first.patch") < inputs.index("patch=mpv/0001-first.patch"),
        "patch blocks follow the same sorted component order",
    )

    # Sorted canonical order means the components list order is not key input;
    # the variants tuple order is.
    shuffled = json.loads(json.dumps(GROUP_JSON))
    shuffled["artifacts"]["libmpv-android"]["components"] = ["libass", "mpv", "ffmpeg"]
    write_group_json(repo, shuffled)
    check(android_key(repo) == key, "components list order must not move the key")
    swapped = json.loads(json.dumps(GROUP_JSON))
    swapped["artifacts"]["libmpv-android"]["variants"] = ["x86_64", "arm64-v8a"]
    write_group_json(repo, swapped)
    check(android_key(repo) != key, "the variants tuple order is part of the key")
    write_group_json(repo, GROUP_JSON)

    # Cross-group isolation, both directions, for every kind of key input.
    versions = json.loads(json.dumps(VERSIONS))
    versions["components"]["libass"]["overrides"]["android"] = {"version": "0.18.4-ndk"}
    write_versions(repo, versions)
    check(android_key(repo) != key, "an android override must move the android key")
    check(keys(repo) == apple_before, "an android override must not move apple keys")
    write_versions(repo, VERSIONS)

    (repo / "patches/mpv/series.apple").write_text("0001-first.patch\n", encoding="utf-8")
    check(android_key(repo) == key, "an apple-only series must not move the android key")
    (repo / "patches/mpv/series.apple").unlink()
    (repo / "patches/mpv/series.android").write_text("0001-first.patch\n", encoding="utf-8")
    changed = android_key(repo)
    check(changed != key, "an android series entry must move the android key")
    check(keys(repo) == apple_before, "an android series must not move apple keys")
    (repo / "patches/mpv/series.android").unlink()
    (repo / "patches/mpv/series.common").write_text(
        "0002-second.patch\n0001-first.patch\n", encoding="utf-8"
    )
    check(
        android_key(repo) != key and keys(repo) != apple_before,
        "a series.common edit must move both groups' keys",
    )
    (repo / "patches/mpv/series.common").write_text(
        "# applied everywhere\n0001-first.patch\n\n0002-second.patch\n", encoding="utf-8"
    )

    (platform / "build.sh").write_text("#!/bin/bash\nbuild # tweak\n", encoding="utf-8")
    check(android_key(repo) != key, "an android driver edit must move the android key")
    check(keys(repo) == apple_before, "an android driver edit must not move apple keys")
    (platform / "build.sh").write_text("#!/bin/bash\nbuild\n", encoding="utf-8")
    base = repo / "Sources/BuildScripts/XCFrameworkBuild/base.swift"
    base.write_text(base.read_text() + "// tweak\n", encoding="utf-8")
    check(android_key(repo) == key, "an apple driver edit must not move the android key")

    (repo / "toolchain" / "android.txt").write_text(
        "ndk=29.0.14206865\ngeneration=2\n", encoding="utf-8"
    )
    check(android_key(repo) != key, "an android toolchain bump must move the android key")
    print("group.json: discovery, multi-component key text and isolation all hold")

# 12. The per-variant record flow: rename, checksum, manifest, gate.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_android_repo(tmp)
    key = android_key(repo)
    stale = run(repo, "stale", "--platform-group", "android").stdout.split()
    check(stale == ["libmpv-android"], f"a fresh android repo is stale: {stale}")
    run(repo, "verify", "--platform-group", "android", expect=1)

    release = repo / "dist" / "release"
    release.mkdir(parents=True)
    # One leg leaves the unkeyed spelling, the other the already-keyed one:
    # record-platform must accept both.
    (release / "libmpv-android-arm64-v8a.tar.gz").write_bytes(b"arm64 bytes")
    # A group.json group records here, so it has to know the run's release,
    # and a legacy group release is never one.
    result = run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "arm64-v8a", "--release-dir", "dist/release", expect=1,
    )
    check("is not a publish run's release" in result.stderr, f"no --release, no record: {result.stderr}")
    result = run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "arm64-v8a", "--release-dir", "dist/release", "--release", "binaries-android",
        expect=1,
    )
    check("take no new assets" in result.stderr, f"a legacy release takes no new asset: {result.stderr}")
    check(not (repo / "artifacts.json").exists(), "a refused record writes nothing")
    result = run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "arm64-v8a", "--release-dir", "dist/release", "--release", RUN_RELEASE,
    )
    arm64_asset = f"libmpv-android-{key}-arm64-v8a.tar.gz"
    check(arm64_asset in result.stdout, "the unkeyed tarball is renamed by content")
    check((release / arm64_asset).is_file(), "the keyed tarball exists")
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["libmpv-android"]
    check(entry["key"] == key, "the android entry records the computed key")
    recorded = entry["prebuilt"]["arm64-v8a"]
    check(recorded["asset"] == arm64_asset, f"recorded asset: {recorded['asset']}")
    check(
        recorded["checksum"] == hashlib.sha256(b"arm64 bytes").hexdigest(),
        "the recorded checksum is the sha256 of the asset file",
    )
    check(recorded["assetBase"] == RUN_BASE, f"the asset is recorded in its run's release: {recorded}")
    check(
        run(repo, "stale", "--platform-group", "android").stdout.split() == ["libmpv-android"],
        "a partially recorded artifact is still stale",
    )
    run(repo, "verify", "--platform-group", "android", expect=1)

    (release / f"libmpv-android-{key}-x86_64.tar.gz").write_bytes(b"x86_64 bytes")
    run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "x86_64", "--release-dir", "dist/release", "--release", RUN_RELEASE,
    )
    check(
        run(repo, "stale", "--platform-group", "android").stdout.strip() == "",
        "nothing is stale once every variant is recorded",
    )
    run(repo, "verify", "--platform-group", "android")
    check(
        manifest_of(repo)["platforms"]["android"]["assetBase"]
        == GROUP_JSON["assetBase"],
        "the android section carries group.json's assetBase",
    )
    check(
        "apple" not in manifest_of(repo)["platforms"],
        "recording android writes no apple section",
    )

    result = run(repo, "render", "--platform-group", "android")
    check("renders no package manifest" in result.stdout, "render is a no-op for android")
    run(repo, "record-frameworks", "--platform-group", "android", "--release", RUN_RELEASE, expect=1)
    run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "armeabi-v7a", "--release-dir", "dist/release", "--release", RUN_RELEASE,
        expect=1,
    )

    # A tampered checksum must fail the gate, and so must an asset located in
    # a legacy group release or outside the repository's releases.
    manifest = manifest_of(repo)
    entry = manifest["platforms"]["android"]["libraries"]["libmpv-android"]
    good = entry["prebuilt"]["x86_64"]["checksum"]
    entry["prebuilt"]["x86_64"]["checksum"] = "zz" + good[2:]
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    result = run(repo, "verify", "--platform-group", "android", expect=1)
    check("checksum" in result.stdout, "a bad recorded checksum is reported")
    entry["prebuilt"]["x86_64"]["checksum"] = good
    for bad_base in (GROUP_JSON["assetBase"], "https://example.invalid/binaries-0123456789ab"):
        entry["prebuilt"]["x86_64"]["assetBase"] = bad_base
        (repo / "artifacts.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        result = run(repo, "verify", "--platform-group", "android", expect=1)
        check("is not a per-run release" in result.stdout, f"assetBase {bad_base} is reported: {result.stdout}")
    entry["prebuilt"]["x86_64"]["assetBase"] = RUN_BASE
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    run(repo, "verify", "--platform-group", "android")

    # A key move restarts the entry: stale variants cannot hide under it.
    (repo / "toolchain" / "android.txt").write_text(
        "ndk=29.0.14206865\ngeneration=2\n", encoding="utf-8"
    )
    new_key = android_key(repo)
    check(new_key != key, "the toolchain bump moved the key")
    (release / "libmpv-android-arm64-v8a.tar.gz").write_bytes(b"arm64 rebuild")
    run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "arm64-v8a", "--release-dir", "dist/release", "--release", RUN_RELEASE,
    )
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["libmpv-android"]
    check(entry["key"] == new_key, "the entry is re-keyed")
    check(
        sorted(entry["prebuilt"]) == ["arm64-v8a"],
        "a key move drops the old variants instead of mixing keys",
    )
    run(repo, "verify", "--platform-group", "android", expect=1)
    print("record-platform: rename, checksum, partial gates and key moves all hold")

# 13. A malformed group.json fails loudly on any invocation.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_android_repo(tmp)
    broken = json.loads(json.dumps(GROUP_JSON))
    broken["artifacts"]["libmpv-android"]["assetPattern"] = "libmpv-android-{key}.tar.gz"
    write_group_json(repo, broken)
    result = run(repo, "keys", expect=1)
    check("assetPattern" in result.stderr, "a pattern without {variant} is rejected")
    print("group.json: malformed definitions are rejected loudly")

# 14. Package artifacts: one asset built from other artifacts of the group plus
#     sources of its own (the mpv-android AAR). Its key covers its inputs' keys
#     and its sources, so a source edit moves it alone.
PACKAGE_GROUP_JSON = json.loads(json.dumps(GROUP_JSON))
PACKAGE_GROUP_JSON["artifacts"]["mpv-android"] = {
    "inputs": ["libmpv-android"],
    "sources": ["platforms/android/lib"],
    "assetPattern": "mpv-android-{key}.aar",
}


def make_package_repo(directory):
    repo = make_android_repo(directory)
    write_group_json(repo, PACKAGE_GROUP_JSON)
    lib = repo / "platforms" / "android" / "lib"
    (lib / "src").mkdir(parents=True)
    (lib / "src" / "glue.cpp").write_text("int glue;\n", encoding="utf-8")
    (lib / "build.gradle.kts").write_text("// build\n", encoding="utf-8")
    (lib / ".gitignore").write_text("/build/\n", encoding="utf-8")
    # Directory sources are what git tracks or would track: the fixture is a
    # repository, and the tracked set is committed.
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    return repo


def package_keys(repo):
    return keys(repo, "--platform-group", "android")


with tempfile.TemporaryDirectory() as tmp:
    repo = make_package_repo(tmp)
    lib = repo / "platforms" / "android" / "lib"
    before = package_keys(repo)
    check(sorted(before) == ["libmpv-android", "mpv-android"], f"android keys: {sorted(before)}")
    check(package_keys(repo) == before, "package keys must be reproducible for the same tree")
    check(
        run(repo, "variants", "--platform-group", "android").stdout.split() == ["arm64-v8a", "x86_64"],
        "a package adds no variants to the group",
    )
    inputs = run(repo, "keys", "--platform-group", "android", "--show-inputs").stdout
    check(f"input=libmpv-android:{before['libmpv-android']}" in inputs, "package inputs carry the input key")
    check("source=platforms/android/lib/src/glue.cpp:" in inputs, "package inputs carry each source file")
    check(
        inputs.index("source=platforms/android/lib/.gitignore")
        < inputs.index("source=platforms/android/lib/build.gradle.kts")
        < inputs.index("source=platforms/android/lib/src/glue.cpp"),
        "source lines are sorted by path",
    )

    (lib / "src" / "glue.cpp").write_text("int glue; // edit\n", encoding="utf-8")
    edited = package_keys(repo)
    check(edited["mpv-android"] != before["mpv-android"], "a source edit must move the package key")
    check(edited["libmpv-android"] == before["libmpv-android"], "a source edit must not move its input's key")
    (lib / "src" / "glue.cpp").write_text("int glue;\n", encoding="utf-8")

    (lib / "src" / "new.kt").write_text("// new\n", encoding="utf-8")
    check(package_keys(repo)["mpv-android"] != before["mpv-android"], "an untracked source file counts")
    (lib / "src" / "new.kt").unlink()
    (lib / "build").mkdir()
    (lib / "build" / "out.aar").write_text("output\n", encoding="utf-8")
    check(package_keys(repo) == before, "ignored build output is not source")

    (repo / "platforms" / "android" / "build.sh").write_text("#!/bin/bash\nbuild # tweak\n", encoding="utf-8")
    moved = package_keys(repo)
    check(
        moved["libmpv-android"] != before["libmpv-android"] and moved["mpv-android"] != before["mpv-android"],
        "a driver edit moves the input key and with it the package key",
    )
    (repo / "platforms" / "android" / "build.sh").write_text("#!/bin/bash\nbuild\n", encoding="utf-8")
    check(package_keys(repo) == before, "restoring the driver restores both keys")
    print("package artifacts: keys cover inputs and sources, and nothing else moves")

# 15. The package record flow, beside the per-variant one.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_package_repo(tmp)
    current = package_keys(repo)
    stale = run(repo, "stale", "--platform-group", "android").stdout.split()
    check(stale == ["libmpv-android", "mpv-android"], f"a fresh package repo is stale: {stale}")

    release = repo / "dist" / "release"
    release.mkdir(parents=True)
    for variant in ("arm64-v8a", "x86_64"):
        (release / f"libmpv-android-{variant}.tar.gz").write_bytes(variant.encode())
        run(
            repo, "record-platform", "--platform-group", "android",
            "--platform", variant, "--release-dir", "dist/release", "--release", RUN_RELEASE,
        )
    check(
        run(repo, "stale", "--platform-group", "android").stdout.split() == ["mpv-android"],
        "recording the tarballs leaves the package stale",
    )
    result = run(
        repo, "record-packages", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", RUN_RELEASE,
    )
    check("not built in this run" in result.stdout, "a package that was not built keeps its entry")

    (release / "mpv-android.aar").write_bytes(b"aar bytes")
    run(
        repo, "record-packages", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", RUN_RELEASE,
    )
    aar = f"mpv-android-{current['mpv-android']}.aar"
    check((release / aar).is_file(), "the unkeyed AAR is renamed by content")
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["mpv-android"]
    check(
        entry == {
            "key": current["mpv-android"],
            "asset": aar,
            "checksum": hashlib.sha256(b"aar bytes").hexdigest(),
            "assetBase": RUN_BASE,
        },
        f"the package entry: {entry}",
    )
    check(run(repo, "stale", "--platform-group", "android").stdout.strip() == "", "nothing is stale")
    run(repo, "verify", "--platform-group", "android")

    manifest = manifest_of(repo)
    manifest["platforms"]["android"]["libraries"]["mpv-android"]["checksum"] = "zz"
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    result = run(repo, "verify", "--platform-group", "android", expect=1)
    check("mpv-android: checksum" in result.stdout, "a bad package checksum is reported")

    (repo / "platforms" / "android" / "lib" / "src" / "glue.cpp").write_text("int moved;\n", encoding="utf-8")
    check(
        run(repo, "stale", "--platform-group", "android").stdout.split() == ["mpv-android"],
        "a glue edit makes the package alone stale",
    )
    result = run(
        repo, "record-packages", "--platform-group", "apple", "--release-dir", "dist/release",
        "--release", RUN_RELEASE,
    )
    check("no package artifacts" in result.stdout, "record-packages is a no-op for a group without packages")
    print("record-packages: rename, checksum, gate and staleness hold")

# 16. Malformed package artifacts fail loudly.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_package_repo(tmp)
    for change, needle in (
        ({"variants": ["arm64-v8a"]}, "no components or variants"),
        ({"assetPattern": "mpv-android-{key}-{variant}.aar"}, "no {variant}"),
        ({"inputs": ["missing"]}, "is not a component artifact"),
        ({"sources": []}, "needs inputs and sources"),
    ):
        broken = json.loads(json.dumps(PACKAGE_GROUP_JSON))
        broken["artifacts"]["mpv-android"].update(change)
        write_group_json(repo, broken)
        result = run(repo, "keys", expect=1)
        check(needle in result.stderr, f"{change} is rejected: {result.stderr.strip()}")
    write_group_json(repo, PACKAGE_GROUP_JSON)
    (repo / "platforms" / "android" / "lib" / "src" / "glue.cpp").unlink()
    (repo / "platforms" / "android" / "lib" / "build.gradle.kts").unlink()
    (repo / "platforms" / "android" / "lib" / ".gitignore").unlink()
    result = run(repo, "keys", "--platform-group", "android", expect=1)
    check("holds no files" in result.stderr, "an empty source directory is rejected")
    print("package artifacts: malformed definitions are rejected loudly")

# 17. Debug symbols: a symbolsPattern publishes one zip per variant beside the
#     artifact's assets. The pattern is not key input; a record fails without
#     the zip; stale and verify demand every variant's.
SYMBOLS_GROUP_JSON = json.loads(json.dumps(GROUP_JSON))
SYMBOLS_GROUP_JSON["artifacts"]["libmpv-android"]["symbolsPattern"] = (
    "libmpv-android-{key}-{variant}-symbols.zip"
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_manifest(repo, manifest):
    (repo / "artifacts.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def record_android_variant(repo, variant, expect=0):
    return run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", variant, "--release-dir", "dist/release", "--release", RUN_RELEASE,
        expect=expect,
    )


def android_stale(repo):
    return run(repo, "stale", "--platform-group", "android").stdout.split()


with tempfile.TemporaryDirectory() as tmp:
    repo = make_android_repo(tmp)
    key = android_key(repo)
    release = repo / "dist" / "release"
    release.mkdir(parents=True)
    # Recorded before the artifact named a pattern: complete then.
    for variant in ("arm64-v8a", "x86_64"):
        (release / f"libmpv-android-{variant}.tar.gz").write_bytes(f"{variant} bytes".encode())
        record_android_variant(repo, variant)
    check(android_stale(repo) == [], "recorded without a symbolsPattern, nothing is stale")

    write_group_json(repo, SYMBOLS_GROUP_JSON)
    check(android_key(repo) == key, "a symbolsPattern is not key input")
    check(android_stale(repo) == ["libmpv-android"], "an entry without its symbols is stale")
    result = run(repo, "verify", "--platform-group", "android", expect=1)
    check(
        "libmpv-android: no symbols asset for arm64-v8a" in result.stdout
        and "libmpv-android: no symbols asset for x86_64" in result.stdout,
        f"verify names every variant's missing symbols: {result.stdout}",
    )

    # A leg that left its binary but not the symbols for it is not recorded.
    before = (repo / "artifacts.json").read_text()
    result = record_android_variant(repo, "arm64-v8a", expect=1)
    check(
        "libmpv-android: arm64-v8a was built without its symbols zip "
        f"libmpv-android-{key}-arm64-v8a-symbols.zip" in result.stderr,
        f"a missing symbols zip fails the record: {result.stderr.strip()}",
    )
    check((repo / "artifacts.json").read_text() == before, "a failed record writes no manifest")

    # The unkeyed spelling is renamed by content, the keyed one accepted.
    (release / "libmpv-android-arm64-v8a-symbols.zip").write_bytes(b"arm64 symbols")
    result = record_android_variant(repo, "arm64-v8a")
    arm64_symbols = f"libmpv-android-{key}-arm64-v8a-symbols.zip"
    check(
        (release / arm64_symbols).is_file()
        and not (release / "libmpv-android-arm64-v8a-symbols.zip").exists(),
        f"the unkeyed symbols zip is renamed by content: {result.stdout}",
    )
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["libmpv-android"]
    check(
        entry["symbols"] == {
            "arm64-v8a": {"asset": arm64_symbols, "checksum": sha256(b"arm64 symbols"), "assetBase": RUN_BASE}
        },
        f"the recorded symbols: {entry.get('symbols')}",
    )
    check(
        sorted(entry["prebuilt"]) == ["arm64-v8a", "x86_64"],
        "a same-key record keeps the other variants",
    )
    check(android_stale(repo) == ["libmpv-android"], "symbols for one variant of two are still stale")
    result = run(repo, "verify", "--platform-group", "android", expect=1)
    check(
        "no symbols asset for x86_64" in result.stdout and "for arm64-v8a" not in result.stdout,
        f"verify names the variant still missing its symbols: {result.stdout}",
    )

    x86_64_symbols = f"libmpv-android-{key}-x86_64-symbols.zip"
    (release / x86_64_symbols).write_bytes(b"x86_64 symbols")
    record_android_variant(repo, "x86_64")
    check(android_stale(repo) == [], "every variant's symbols recorded: nothing is stale")
    run(repo, "verify", "--platform-group", "android")

    good = manifest_of(repo)
    for mutate, needle in (
        (lambda entry: entry["symbols"]["x86_64"].update(checksum="zz"), "symbols x86_64: checksum 'zz'"),
        (
            lambda entry: entry["symbols"]["x86_64"].update(
                asset="libmpv-android-000000000000-x86_64-symbols.zip"
            ),
            "symbols x86_64 asset 'libmpv-android-000000000000-x86_64-symbols.zip' does not match",
        ),
        (lambda entry: entry["symbols"].pop("x86_64"), "no symbols asset for x86_64"),
        (lambda entry: entry.pop("symbols"), "no symbols asset for arm64-v8a"),
    ):
        manifest = json.loads(json.dumps(good))
        mutate(manifest["platforms"]["android"]["libraries"]["libmpv-android"])
        write_manifest(repo, manifest)
        result = run(repo, "verify", "--platform-group", "android", expect=1)
        check(needle in result.stdout, f"verify reports {needle!r}: {result.stdout}")
        check(android_stale(repo) == ["libmpv-android"], f"{needle!r} makes the artifact stale")
    write_manifest(repo, good)

    # A key move restarts the entry, symbols included: an old key's zip can
    # never stand in for a new key's variant.
    (repo / "toolchain" / "android.txt").write_text("ndk=29.0.14206865\ngeneration=2\n", encoding="utf-8")
    new_key = android_key(repo)
    (release / "libmpv-android-arm64-v8a.tar.gz").write_bytes(b"arm64 rebuild")
    (release / "libmpv-android-arm64-v8a-symbols.zip").write_bytes(b"arm64 symbols rebuild")
    record_android_variant(repo, "arm64-v8a")
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["libmpv-android"]
    check(
        entry["symbols"] == {
            "arm64-v8a": {
                "asset": f"libmpv-android-{new_key}-arm64-v8a-symbols.zip",
                "checksum": sha256(b"arm64 symbols rebuild"),
                "assetBase": RUN_BASE,
            }
        },
        f"a key move drops the old key's symbols: {entry['symbols']}",
    )
    print("symbols: record renames and checksums them; stale and verify demand every variant's")

# 18. A package's symbols come per input variant (the AAR ships one native
#     tree per tarball ABI), and publish-assets uploads them with the rest.
#     publish-assets runs against a fake `gh` that serves canned releases
#     and logs every call: nothing here reaches GitHub.
SYMBOLS_PACKAGE_GROUP_JSON = json.loads(json.dumps(PACKAGE_GROUP_JSON))
SYMBOLS_PACKAGE_GROUP_JSON["artifacts"]["mpv-android"]["symbolsPattern"] = (
    "mpv-android-{key}-{variant}-symbols.zip"
)

# The releases it serves: {tag: {"assets": [[name, sha256], ...],
# "prerelease": bool (default true)}}.
FAKE_GH = """#!{python}
import json
import os
import sys

args = sys.argv[1:]
with open(os.environ["FAKE_GH_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\\n")
with open(os.environ["FAKE_GH_RELEASES"], encoding="utf-8") as state:
    releases = json.load(state)
if args[:2] == ["release", "list"]:
    for tag, release in releases.items():
        sys.stdout.write(tag + " " + str(release.get("prerelease", True)).lower() + "\\n")
elif args[:2] == ["release", "view"]:
    release = releases.get(args[2])
    if release is None:
        sys.exit(1)
    if "--json" in args:
        jq = args[args.index("--jq") + 1]
        for name, digest in release["assets"]:
            sys.stdout.write(name + (" sha256:" + digest if "digest" in jq else "") + "\\n")
"""


def make_fake_gh(bin_dir):
    (bin_dir / "gh").write_text(FAKE_GH.format(python=sys.executable), encoding="utf-8")
    (bin_dir / "gh").chmod(0o755)


def with_fake_gh(repo, bin_dir, releases, *args, expect=0):
    """keys.py `args` against the fake gh serving `releases`; (result, its gh calls)."""
    log = bin_dir / "gh.log"
    log.write_text("", encoding="utf-8")
    state = bin_dir / "releases.json"
    state.write_text(json.dumps(releases), encoding="utf-8")
    result = run(
        repo, *args,
        expect=expect,
        env={
            "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
            "FAKE_GH_LOG": str(log),
            "FAKE_GH_RELEASES": str(state),
        },
    )
    return result, [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]


def publish_assets(repo, bin_dir, releases, expect=0, release=RUN_RELEASE):
    """publish-assets against the fake gh; (result, uploaded names, releases listed)."""
    result, calls = with_fake_gh(
        repo, bin_dir, releases,
        "publish-assets", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", release,
        expect=expect,
    )
    uploaded = []
    listed = []
    for args in calls:
        check(args[:2] in (["release", "view"], ["release", "upload"]), f"unexpected gh call: {args}")
        if args[:2] == ["release", "view"] and "--json" in args:
            listed.append(args[2])
        if args[:2] == ["release", "upload"]:
            check(args[2] == release, f"uploads go only to the run's own release: {args}")
            uploaded += [Path(path).name for path in args[3:]]
    return result, sorted(uploaded), listed


def assets(*pairs):
    return {"assets": [list(pair) for pair in pairs]}


with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as bin_tmp:
    repo = make_package_repo(tmp)
    unpatterned = package_keys(repo)
    write_group_json(repo, SYMBOLS_PACKAGE_GROUP_JSON)
    current = package_keys(repo)
    check(current == unpatterned, "a package's symbolsPattern is not key input")
    key = current["mpv-android"]

    release = repo / "dist" / "release"
    release.mkdir(parents=True)
    for variant in ("arm64-v8a", "x86_64"):
        (release / f"libmpv-android-{variant}.tar.gz").write_bytes(variant.encode())
        record_android_variant(repo, variant)
    (release / "mpv-android.aar").write_bytes(b"aar bytes")
    result = run(
        repo, "record-packages", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", RUN_RELEASE, expect=1,
    )
    check(
        f"mpv-android: arm64-v8a was built without its symbols zip mpv-android-{key}-arm64-v8a-symbols.zip"
        in result.stderr,
        f"an AAR without its symbols is not recorded: {result.stderr.strip()}",
    )
    check(
        "mpv-android" not in manifest_of(repo)["platforms"]["android"]["libraries"],
        "a failed package record writes no entry",
    )

    (release / "mpv-android-arm64-v8a-symbols.zip").write_bytes(b"arm64 symbols")
    (release / f"mpv-android-{key}-x86_64-symbols.zip").write_bytes(b"x86_64 symbols")
    run(
        repo, "record-packages", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", RUN_RELEASE,
    )
    aar = f"mpv-android-{key}.aar"
    arm64_symbols = f"mpv-android-{key}-arm64-v8a-symbols.zip"
    x86_64_symbols = f"mpv-android-{key}-x86_64-symbols.zip"
    entry = manifest_of(repo)["platforms"]["android"]["libraries"]["mpv-android"]
    check(
        entry == {
            "key": key,
            "asset": aar,
            "checksum": sha256(b"aar bytes"),
            "assetBase": RUN_BASE,
            "symbols": {
                "arm64-v8a": {"asset": arm64_symbols, "checksum": sha256(b"arm64 symbols"), "assetBase": RUN_BASE},
                "x86_64": {"asset": x86_64_symbols, "checksum": sha256(b"x86_64 symbols"), "assetBase": RUN_BASE},
            },
        },
        f"the package entry carries its inputs' variants' symbols: {entry}",
    )
    check(not (release / "mpv-android-arm64-v8a-symbols.zip").exists(), "the unkeyed package symbols are renamed")
    check(android_stale(repo) == [], "a package recorded with its symbols is not stale")
    run(repo, "verify", "--platform-group", "android")

    bin_dir = Path(bin_tmp)
    make_fake_gh(bin_dir)
    tarball = f"libmpv-android-{current['libmpv-android']}-arm64-v8a.tar.gz"
    # An earlier attempt of the same run uploaded two of the assets already.
    earlier = assets((tarball, sha256(b"arm64-v8a")), (arm64_symbols, sha256(b"arm64 symbols")))
    result, uploaded, _ = publish_assets(repo, bin_dir, {RUN_RELEASE: earlier})
    check(
        uploaded == sorted([aar, f"libmpv-android-{current['libmpv-android']}-x86_64.tar.gz", x86_64_symbols]),
        f"publish-assets uploads the unpublished assets, symbols included: {uploaded}",
    )
    check(
        f"published {arm64_symbols} in {RUN_RELEASE} (bytes verified)" in result.stdout,
        f"a published symbols zip is verified against the rebuilt bytes: {result.stdout}",
    )

    result, uploaded, _ = publish_assets(
        repo, bin_dir,
        {RUN_RELEASE: assets((tarball, sha256(b"arm64-v8a")), (arm64_symbols, "0" * 64))},
        expect=1,
    )
    check(
        f"{arm64_symbols}: rebuilt bytes" in result.stderr and uploaded == [],
        f"a symbols zip whose rebuilt bytes moved stops the upload: {result.stderr.strip()}",
    )

    result, uploaded, _ = publish_assets(repo, bin_dir, {"binaries-android": earlier}, expect=1)
    check(
        f"release {RUN_RELEASE} does not exist" in result.stderr and uploaded == [],
        f"nothing is uploaded before open-release created the run's release: {result.stderr.strip()}",
    )
    result, uploaded, _ = publish_assets(
        repo, bin_dir, {"binaries-android": earlier}, release="binaries-android", expect=1
    )
    check(
        "take no new assets" in result.stderr and uploaded == [],
        f"a legacy group release is never a run's release: {result.stderr.strip()}",
    )

    (release / x86_64_symbols).unlink()
    result, uploaded, _ = publish_assets(repo, bin_dir, {RUN_RELEASE: earlier}, expect=1)
    check(
        f"mpv-android: {x86_64_symbols}" in result.stderr and uploaded == [],
        f"a symbols zip neither built nor published stops the upload: {result.stderr.strip()}",
    )
    print("symbols: packages record their inputs' variants; publish-assets uploads and verifies them")

# 18b. Each asset resolves to its own release. Tarballs recorded before
#      per-run releases (no assetBase) are served from the group's legacy
#      release; the AAR a later run recorded, from that run's. publish-assets
#      checks a rebuilt copy of a published asset against the release its
#      entry names (verify-on-skip) and uploads to the run's own alone.
with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as bin_tmp:
    repo = make_package_repo(tmp)
    current = package_keys(repo)
    release = repo / "dist" / "release"
    release.mkdir(parents=True)
    for variant in ("arm64-v8a", "x86_64"):
        (release / f"libmpv-android-{variant}.tar.gz").write_bytes(variant.encode())
        record_android_variant(repo, variant)
    manifest = manifest_of(repo)
    for recorded in manifest["platforms"]["android"]["libraries"]["libmpv-android"]["prebuilt"].values():
        del recorded["assetBase"]
    write_manifest(repo, manifest)
    (release / "mpv-android.aar").write_bytes(b"aar bytes")
    run(
        repo, "record-packages", "--platform-group", "android", "--release-dir", "dist/release",
        "--release", LATER_RELEASE,
    )
    check(android_stale(repo) == [], "a tarball entry without assetBase is still published")
    run(repo, "verify", "--platform-group", "android")
    section = manifest_of(repo)["platforms"]["android"]
    check(
        all("assetBase" not in recorded for recorded in section["libraries"]["libmpv-android"]["prebuilt"].values()),
        "recording the package leaves the old tarball entries as they were",
    )
    check(section["libraries"]["mpv-android"]["assetBase"] == LATER_BASE, "the AAR is in the later run's release")

    bin_dir = Path(bin_tmp)
    make_fake_gh(bin_dir)
    arm64 = f"libmpv-android-{current['libmpv-android']}-arm64-v8a.tar.gz"
    x86_64 = f"libmpv-android-{current['libmpv-android']}-x86_64.tar.gz"
    aar = f"mpv-android-{current['mpv-android']}.aar"
    legacy = assets((arm64, sha256(b"arm64-v8a")), (x86_64, sha256(b"x86_64")))
    result, uploaded, listed = publish_assets(
        repo, bin_dir, {"binaries-android": legacy, LATER_RELEASE: assets()}, release=LATER_RELEASE
    )
    check(uploaded == [aar], f"only the run's own new asset is uploaded: {uploaded}")
    check(
        sorted(set(listed)) == sorted(["binaries-android", LATER_RELEASE]),
        f"the old entries are looked up in the group's legacy release: {listed}",
    )
    check(
        f"published {arm64} in binaries-android (bytes verified)" in result.stdout,
        f"verify-on-skip checks an old entry's asset in the legacy release: {result.stdout}",
    )

    # Verify-on-skip reads the recorded release, not some other copy: the
    # legacy release's bytes differ even though the run's release holds a
    # matching copy under the same name.
    result, uploaded, _ = publish_assets(
        repo, bin_dir,
        {
            "binaries-android": assets((arm64, "0" * 64), (x86_64, sha256(b"x86_64"))),
            LATER_RELEASE: assets((arm64, sha256(b"arm64-v8a"))),
        },
        release=LATER_RELEASE, expect=1,
    )
    check(
        f"{arm64}: rebuilt bytes" in result.stderr and "published in binaries-android" in result.stderr
        and uploaded == [],
        f"verify-on-skip uses the recorded release: {result.stderr.strip()}",
    )

    # An old entry's asset missing from its recorded release is a dead URL; it
    # is never re-uploaded anywhere to cover for it.
    result, uploaded, _ = publish_assets(
        repo, bin_dir,
        {"binaries-android": assets((x86_64, sha256(b"x86_64"))), LATER_RELEASE: assets()},
        release=LATER_RELEASE, expect=1,
    )
    check(
        f"{arm64} (recorded in binaries-android, which does not hold it)" in result.stderr and uploaded == [],
        f"an old asset missing from its release fails the publish: {result.stderr.strip()}",
    )

    # A third run that rebuilt the AAR's bytes finds them in the later run's
    # release, where its entry points, and uploads nothing to its own.
    result, uploaded, listed = publish_assets(
        repo, bin_dir,
        {"binaries-android": legacy, LATER_RELEASE: assets((aar, sha256(b"aar bytes"))), RUN_RELEASE: assets()},
    )
    check(
        uploaded == [] and f"published {aar} in {LATER_RELEASE} (bytes verified)" in result.stdout,
        f"a new entry is verified in its own run's release: {result.stdout}",
    )
    print("locations: each asset is listed, verified and uploaded in its own release")

# 18c. The run's release. prune-releases deletes every per-run prerelease that
#      no artifacts.json or Package.swift on any ref names, and nothing else;
#      open-release creates the run's prerelease on its commit; release-notes
#      lists that commit and the release's assets.
def git(repo, *args):
    return subprocess.run(
        ["git", "-c", "user.name=test", "-c", "user.email=test@example.invalid", *args],
        cwd=repo, check=True, capture_output=True, text=True,
    ).stdout


BRANCH_RELEASE = "binaries-0000000b4a4c"
ORPHAN_RELEASE = "binaries-00000000dead"
FINAL_RELEASE = "binaries-0000000f0e1d"

with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryDirectory() as bin_tmp, \
        tempfile.TemporaryDirectory() as clone_tmp:
    repo = make_repo(tmp)
    (repo / ".gitignore").write_text("dist/\n", encoding="utf-8")
    git(repo, "init", "-q", "-b", "main")
    publish(repo)
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "binaries in the first run's release")
    # Every library rebuilt by a later run: main's newest manifest no longer
    # names the first run's release, but its history does.
    publish(repo, release=LATER_RELEASE)
    check(RUN_RELEASE not in (repo / "artifacts.json").read_text(), "the newest manifest names only the later run")
    git(repo, "commit", "-q", "-am", "binaries in the later run's release")
    git(repo, "checkout", "-q", "-b", "side")
    (repo / "artifacts.json").write_text(
        (repo / "artifacts.json").read_text().replace(LATER_RELEASE, BRANCH_RELEASE), encoding="utf-8"
    )
    git(repo, "commit", "-q", "-am", "a branch's manifest")
    git(repo, "checkout", "-q", "main")

    bin_dir = Path(bin_tmp)
    make_fake_gh(bin_dir)
    releases = {
        "binaries-apple": assets(),
        "1.0.26": assets(),
        RUN_RELEASE: assets(),
        LATER_RELEASE: assets(),
        BRANCH_RELEASE: assets(),
        ORPHAN_RELEASE: assets(),
        FINAL_RELEASE: {"assets": [], "prerelease": False},
    }
    result, calls = with_fake_gh(repo, bin_dir, releases, "prune-releases")
    deleted = [args for args in calls if args[:2] == ["release", "delete"]]
    check(
        [args[2] for args in deleted] == [ORPHAN_RELEASE],
        f"only the release no manifest names is deleted: {deleted}",
    )
    check(all("--cleanup-tag" in args for args in deleted), "its tag goes with it")

    clone = Path(clone_tmp) / "clone"
    subprocess.run(
        ["git", "clone", "-q", "--depth", "1", f"file://{repo}", str(clone)], check=True, capture_output=True
    )
    result, calls = with_fake_gh(clone, bin_dir, releases, "prune-releases", expect=1)
    check(
        "shallow" in result.stderr and not any(args[:2] == ["release", "delete"] for args in calls),
        f"a shallow clone cannot see the history, so it deletes nothing: {result.stderr.strip()}",
    )

    commit = git(repo, "rev-parse", "HEAD").strip()
    tag = f"binaries-{commit[:12]}"
    result, calls = with_fake_gh(repo, bin_dir, releases, "open-release", "--release", tag, "--commit", commit)
    created = [args for args in calls if args[:2] == ["release", "create"]]
    check(
        len(created) == 1 and created[0][2] == tag and "--prerelease" in created[0]
        and created[0][created[0].index("--target") + 1] == commit,
        f"open-release creates the commit's prerelease: {created}",
    )
    result, calls = with_fake_gh(
        repo, bin_dir, {**releases, tag: assets()}, "open-release", "--release", tag, "--commit", commit,
        expect=1,
    )
    check(
        "exists already" in result.stderr and not any(args[:2] == ["release", "create"] for args in calls),
        f"a release that survived the prune is published and never added to: {result.stderr.strip()}",
    )
    result, _ = with_fake_gh(
        repo, bin_dir, releases, "open-release", "--release", RUN_RELEASE, "--commit", commit, expect=1
    )
    check("is not the release of commit" in result.stderr, "the tag must be the commit's")

    result, calls = with_fake_gh(
        repo, bin_dir, {tag: assets(("b.zip", "1" * 64), ("a.zip", "2" * 64))},
        "release-notes", "--release", tag, "--commit", commit,
    )
    edited = [args for args in calls if args[:2] == ["release", "edit"]]
    notes = edited[0][edited[0].index("--notes") + 1] if edited else ""
    check(
        f"/commit/{commit}" in notes and "`a.zip`" in notes and notes.index("`a.zip`") < notes.index("`b.zip`"),
        f"the notes name the source commit and list the assets: {notes!r}",
    )
    print("releases: orphans pruned, history and branches respected, open and notes hold")

# 19. A malformed symbolsPattern fails loudly.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_package_repo(tmp)
    for artifact, change in (
        ("libmpv-android", {"symbolsPattern": "libmpv-android-{variant}-symbols.zip"}),
        ("libmpv-android", {"symbolsPattern": "libmpv-android-{key}-symbols.zip"}),
        ("libmpv-android", {"symbolsPattern": "libmpv-android-{key}-{variant}-symbols.tar.gz"}),
        ("mpv-android", {"symbolsPattern": "mpv-android-{key}-symbols.zip"}),
        (
            "libmpv-android",
            {"assetPattern": "libmpv-android-{key}-{variant}.zip",
             "symbolsPattern": "libmpv-android-{key}-{variant}.zip"},
        ),
        # Different keyed names, one unkeyed name (libmpv-android-<variant>.zip).
        (
            "libmpv-android",
            {"assetPattern": "libmpv-android-{key}-{variant}.zip",
             "symbolsPattern": "libmpv-android-debug-{key}-{variant}.zip"},
        ),
    ):
        broken = json.loads(json.dumps(SYMBOLS_PACKAGE_GROUP_JSON))
        broken["artifacts"][artifact].update(change)
        write_group_json(repo, broken)
        result = run(repo, "keys", expect=1)
        check(
            f"artifact {artifact!r} symbolsPattern must contain" in result.stderr,
            f"{artifact} {change} is rejected: {result.stderr.strip()}",
        )
    print("symbols: malformed patterns are rejected loudly")

# 20. The real group.json files carry the symbols patterns Plezy downloads, and
#     scripts/symbols.py, which writes those zips' bytes, keys their artifacts.
for group, artifact, pattern, listed_in in (
    ("android", "mpv-android", "mpv-android-{key}-{variant}-symbols.zip", "sources"),
    ("linux", "libmpv-linux", "libmpv-linux-{key}-{variant}-symbols.zip", "driver"),
    ("windows", "libmpv-windows", "libmpv-windows-{key}-{variant}-symbols.zip", "driver"),
):
    path = root / "platforms" / group / "group.json"
    if not path.is_file():
        print(f"real {group} symbols: skipped (no group.json)")
        continue
    data = json.loads(path.read_text(encoding="utf-8"))
    spec = data["artifacts"][artifact]
    check(spec.get("symbolsPattern") == pattern, f"{artifact} symbolsPattern: {spec.get('symbolsPattern')!r}")
    files = data["driver"] if listed_in == "driver" else spec.get("sources", [])
    check("scripts/symbols.py" in files, f"{group}: scripts/symbols.py is in {artifact}'s {listed_in}")
print("real group.json: the symbols patterns match the consumer's")

# 21. scripts/licenses.py decides which licence texts every group.json
#     artifact carries, so each of those groups' driver lists it.
for path in sorted((root / "platforms").glob("*/group.json")):
    data = json.loads(path.read_text(encoding="utf-8"))
    check("scripts/licenses.py" in data["driver"], f"{path.parent.name}: scripts/licenses.py is in its driver")
print("real group.json: the licence collector keys every group")

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)

print("\nall checks passed")
PY
