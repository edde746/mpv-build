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

    write_versions(repo, VERSIONS)

    (repo / "toolchain").mkdir()
    (repo / "toolchain" / "apple.txt").write_text("generation=1\n", encoding="utf-8")

    build_scripts = repo / "Sources" / "BuildScripts"
    (build_scripts / "XCFrameworkBuild").mkdir(parents=True)
    (build_scripts / "Package.swift").write_text("// build scripts package\n", encoding="utf-8")
    (build_scripts / "XCFrameworkBuild" / "base.swift").write_text("// base\n", encoding="utf-8")
    (build_scripts / "XCFrameworkBuild" / "main.swift").write_text("// main\n", encoding="utf-8")

    # mpv and ffmpeg carry patches; libass deliberately has no patches/ dir at
    # all, mirroring the real tree.
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


def publish(repo, libraries=LIBRARIES, platforms=PLATFORMS):
    """One full pass: build every leg, name artifacts, record, render."""
    for platform in platforms:
        build_artifacts(repo, libraries=libraries, platforms=[platform])
        run(repo, "record-platform", "--platform", platform, "--release-dir", "dist/release")
    run(repo, "record-frameworks", "--release-dir", "dist/release")
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

# 2. The committed manifest still describes the assets the old binaries.json
#    described: same libraries, same keys, same assets, same checksums.
committed = root / "artifacts.json"
if committed.is_file():
    manifest = json.loads(committed.read_text(encoding="utf-8"))
    check(manifest.get("schema") == 2, "committed artifacts.json is schema 2")
    apple = manifest.get("platforms", {}).get("apple", {})
    check(apple.get("assetBase") == ASSET_BASE, f"apple assetBase: {apple.get('assetBase')!r}")
    old_blob = None
    try:
        deleting = subprocess.run(
            ["git", "log", "--format=%H", "-n1", "--diff-filter=D",
             "--", "Sources/BuildScripts/binaries.json"],
            capture_output=True, text=True, cwd=root, timeout=30,
        ).stdout.strip()
        if deleting:
            shown = subprocess.run(
                ["git", "show", f"{deleting}^:Sources/BuildScripts/binaries.json"],
                capture_output=True, text=True, cwd=root, timeout=30,
            )
            if shown.returncode == 0:
                old_blob = json.loads(shown.stdout)
    except (OSError, subprocess.TimeoutExpired, ValueError):
        old_blob = None
    if old_blob is not None:
        check(
            apple.get("libraries") == old_blob["libraries"],
            "the apple section must carry the old binaries.json libraries verbatim",
        )
        print("migration: apple section matches the deleted binaries.json")
    else:
        print("migration: skipped (binaries.json history not reachable)")
else:
    print("migration: skipped (artifacts.json not committed yet)")

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

# 6. The group flag parses before and after the verb; unknown groups do not.
with tempfile.TemporaryDirectory() as tmp:
    repo = make_repo(tmp)
    plain = keys(repo)
    check(keys(repo, "--platform-group", "apple") == plain, "trailing --platform-group works")
    result = run(repo, "--platform-group", "apple", "keys")
    check(json.loads(result.stdout) == plain, "leading --platform-group works")
    run(repo, "keys", "--platform-group", "haiku", expect=2)
    print("--platform-group: both positions accepted, unknown groups rejected")

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
        entry["prebuilt"]["tvos"] == f"FFmpeg-all-{expected_key}-tvos.zip",
        f"prebuilt asset name: {entry['prebuilt']['tvos']}",
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
        f'url: "{ASSET_BASE}/{asset}"' in package,
        "Package.swift points at the sharded content-addressed asset",
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
        good_package.replace("/binaries-apple/Libmpv-", "/binaries-apple/Libmpv-stale-"),
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
            f'url: "{ASSET_BASE}/{libmpv["asset"]}",\n'
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
    result = run(repo, "record-frameworks", "--release-dir", "dist/release", expect=1)
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
    publish(repo, libraries={"libmpv": ["libmpv"]})
    after = apple_libraries(repo)
    check(
        after["FFmpeg"] == before["FFmpeg"],
        "an untouched library's entry survives a partial publish",
    )
    check(after["libmpv"] != before["libmpv"], "the rebuilt library's entry is replaced")
    check(
        manifest_of(repo)["platforms"]["android"]
        == {"assetBase": "https://example.invalid", "libraries": {}},
        "another group's section survives an apple publish verbatim",
    )
    run(repo, "verify")
    print("incremental publish: only the stale library's entry changes")

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
    check(
        keys(repo, "--platform-group", "android", "--debug") == android,
        "the debug flag must not move a group.json key",
    )

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
    result = run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "arm64-v8a", "--release-dir", "dist/release",
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
    check(
        run(repo, "stale", "--platform-group", "android").stdout.split() == ["libmpv-android"],
        "a partially recorded artifact is still stale",
    )
    run(repo, "verify", "--platform-group", "android", expect=1)

    (release / f"libmpv-android-{key}-x86_64.tar.gz").write_bytes(b"x86_64 bytes")
    run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "x86_64", "--release-dir", "dist/release",
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
    run(repo, "record-frameworks", "--platform-group", "android", expect=1)
    run(
        repo, "record-platform", "--platform-group", "android",
        "--platform", "armeabi-v7a", "--release-dir", "dist/release", expect=1,
    )

    # A tampered checksum must fail the gate.
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
        "--platform", "arm64-v8a", "--release-dir", "dist/release",
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

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)

print("\nall checks passed")
PY
