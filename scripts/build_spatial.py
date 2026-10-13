#!/usr/bin/env python3
"""Build ao_spatial's private Swift/Rust dependency for one Apple slice.

No release XCFramework or SwiftPM resolution is involved. The source commit,
Cargo.lock and Rust toolchain are all pinned and included in libmpv's key.
"""

import argparse
import base64
import os
from pathlib import Path
import shutil
import subprocess

import patches


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def build(root, source, output, mpv, target, sdk, rust_target):
    pin = patches.component_pins("spatialaudiobridge", "apple")
    if not source.exists():
        patches.cmd_fetch("spatialaudiobridge", "apple", str(source))
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin["commit"]:
        raise SystemExit(f"{source}: expected {pin['commit']}, found {revision}")

    toolchain = dict(line.split("=", 1) for line in (root / "toolchain/apple.txt").read_text().splitlines()
                     if line and not line.startswith("#"))
    rust = toolchain["rust"]
    rustup = Path.home() / ".cargo/bin/rustup"
    if not rustup.exists():
        rustup = Path(shutil.which("rustup") or "rustup")
    output.mkdir(parents=True, exist_ok=True)
    cargo_target = output / "cargo-target"
    manifest = source / "Rust/SpatialAudioCore/Cargo.toml"
    environment = dict(os.environ, CARGO_TARGET_DIR=str(cargo_target),
                       MACOSX_DEPLOYMENT_TARGET="11.0", IPHONEOS_DEPLOYMENT_TARGET="14.0",
                       TVOS_DEPLOYMENT_TARGET="14.0", SDKROOT=str(sdk), ZERO_AR_DATE="1")
    environment.pop("RUSTFLAGS", None)
    sysroot = subprocess.check_output([str(rustup), "run", rust, "rustc", "--print", "sysroot"], text=True).strip()
    environment["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join([
        f"--remap-path-prefix={source}=/spatial-audio-bridge",
        f"--remap-path-prefix={Path.home() / '.cargo'}=/cargo",
        f"--remap-path-prefix={sysroot}=/rust",
    ])
    try:
        run(rustup, "run", rust, "cargo", "build", "--locked", "--release", "-j", "2",
            "--manifest-path", manifest, "--target", rust_target, env=environment)
        shutil.copy2(cargo_target / rust_target / "release/libspatial_audio_core.a", output)
    finally:
        shutil.rmtree(cargo_target, ignore_errors=True)

    # Only the renderer path is linked; HLS, networking and SwiftPM resource
    # accessors are not part of this audio output.
    names = ["Types", "Decoder", "APACEncoder", "SampleBuffer", "IEC61937Unpacker",
             "SpatialAudioRenderer", "SpatialAudioTranscoder", "FragmentTrim"]
    sources = [source / f"Sources/SpatialAudioBridge/{name}.swift" for name in names]
    sources += [source / "Sources/SpatialAudioPlayback/AtmosOutputActivator.swift", mpv / "audio/out/ao_spatial.swift"]
    silent = source / "Sources/SpatialAudioPlayback/Resources/silent-atmos.mp4"
    resources = output / "SpatialResources.swift"
    resources.write_text('import Foundation\nlet mpSpatialSilentAtmos = Data(base64Encoded: "' +
                         base64.b64encode(silent.read_bytes()).decode() + '")!\n')
    run("xcrun", "swiftc", "-swift-version", "6", "-O", "-whole-module-optimization",
        "-parse-as-library", "-emit-library", "-static", "-module-name", "MPVSpatial",
        "-file-prefix-map", f"{root}=/mpv-build", "-file-prefix-map", f"{source}=/spatial-audio-bridge",
        "-target", target, "-sdk", sdk, "-I", source / "Rust/SpatialAudioCore/include",
        *sources, resources, "-o", output / "libmpv_spatial.a", env=environment)
    # libmpv's Meson link uses this for its executable. The install step folds
    # both archives into libmpv.a, so consumers need no extra linker options.
    pkgconfig = output / "pkgconfig"
    pkgconfig.mkdir(exist_ok=True)
    escaped = str(output).replace(" ", "\\ ")
    (pkgconfig / "mpv-spatial.pc").write_text(
        f"Name: mpv-spatial\nDescription: SpatialAudioBridge renderer for libmpv\nVersion: {pin['version']}\n"
        f"Libs: -L{escaped} -lmpv_spatial -lspatial_audio_core -L/usr/lib/swift -framework Accelerate\n")
    shutil.copy2(source / "Sources/SpatialAudioBridge/Resources/ThirdPartyNotices.txt", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mpv", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--rust-target", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    build(root, args.source.resolve(), args.output.resolve(), args.mpv.resolve(),
          args.target, args.sdk, args.rust_target)


if __name__ == "__main__":
    main()
