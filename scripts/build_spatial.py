#!/usr/bin/env python3
"""Build ao_spatial's private Swift/Rust dependency for one Apple slice.

No release XCFramework or SwiftPM resolution is involved. The source commit,
Cargo.lock and Rust toolchain are all pinned and included in libmpv's key.
"""

import argparse
import base64
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

import patches


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def weaken_personality(path):
    """Mark the sole external personality definition weak in a thin Mach-O."""
    data = bytearray(path.read_bytes())
    if struct.unpack_from("<I", data)[0] != 0xFEEDFACF:
        raise ValueError(f"{path}: expected a thin 64-bit Mach-O object")
    offset = 32
    matches = []
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 2:  # LC_SYMTAB
            symbols, count, strings, _ = struct.unpack_from("<IIII", data, offset + 8)
            for index in range(count):
                entry = symbols + index * 16
                name, kind, _, flags = struct.unpack_from("<IBBH", data, entry)
                start = strings + name
                if data[start:data.index(0, start)] != b"_rust_eh_personality":
                    continue
                if kind & 0x0F != 0x0F:  # N_EXT | N_SECT
                    raise ValueError("rust_eh_personality is not an external definition")
                matches.append((entry + 6, flags))
        offset += size
    if len(matches) != 1:
        raise ValueError(f"{path}: expected one personality definition, found {len(matches)}")
    entry, flags = matches[0]
    struct.pack_into("<H", data, entry, flags | 0x0080)  # N_WEAK_DEF
    path.write_bytes(data)


def prelink_core(archive, output, target, sdk, environment):
    # LibTrueHD's build.sh uses this approach to coexist with libdovi: localize
    # Rust/std globals, but share a weak personality so compact unwind needs
    # only one Rust personality alongside Objective-C and C++. SAB catches
    # decoder panics inside its FFI boundary; its sources are not modified.
    match = re.fullmatch(r"(arm64|x86_64)-apple-(macos|ios|tvos)([\d.]+)(-simulator)?", target)
    if not match:
        raise ValueError(f"Unsupported spatial target: {target}")
    arch, platform, minimum, simulator = match.groups()
    platform += simulator or ""
    version = subprocess.check_output(["xcrun", "--sdk", str(sdk), "--show-sdk-version"], text=True).strip()
    members = subprocess.check_output(["xcrun", "ar", "-t", str(archive)], text=True).splitlines()
    if len(members) != len(set(members)):
        raise ValueError(f"{archive}: duplicate archive member names would lose objects during extraction")
    with tempfile.TemporaryDirectory(prefix="prelink-", dir=output) as temporary:
        work = Path(temporary)
        objects = work / "objects"
        objects.mkdir()
        run("xcrun", "ar", "-x", archive, cwd=objects)
        exports = work / "exports.txt"
        exports.write_text("_sab_*\n_rust_eh_personality\n")
        combined = work / "SpatialAudioCore.o"
        run("xcrun", "ld", "-r", "-arch", arch, "-platform_version", platform, minimum, version,
            "-exported_symbols_list", exports, "-o", combined, *sorted(objects.glob("*.o")))
        run("xcrun", "strip", "-S", combined)
        run("xcrun", "vtool", "-show-build", combined)
        weaken_personality(combined)
        destination = output / "libspatial_audio_core.a"
        run("xcrun", "libtool", "-static", "-o", destination, combined, env=environment)
        exported = subprocess.check_output(["xcrun", "nm", "-gUj", str(combined)], text=True).splitlines()
    symbols = subprocess.check_output(["xcrun", "nm", "-m", str(destination)], text=True)
    if not re.search(r"weak external (?:\[no dead strip\] )?_rust_eh_personality$", symbols, re.MULTILINE):
        raise ValueError(f"{target}: missing weak external rust_eh_personality")
    if not any(name.startswith("_sab_") for name in exported) or any(
        not name.startswith("_sab_") and name != "_rust_eh_personality" for name in exported
    ):
        raise ValueError(f"{target}: unexpected exported Rust symbols: {exported}")
    print(f"{target}: prelinked core exports {', '.join(exported)}; personality is weak", flush=True)


def compile_bridge(root, source, output, mpv, target, sdk, environment):
    """Compile ao_spatial's Swift side with the renderer sources it uses."""
    # Only the passthrough path is linked; HLS, networking and SwiftPM
    # resource accessors are not part of this audio output.
    names = ["Types", "Decoder", "APACEncoder", "SampleBuffer", "IEC61937Unpacker",
             "SpatialAudioRenderer", "SpatialAudioPassthroughSink", "SpatialAudioTranscoder", "FragmentTrim"]
    sources = [source / f"Sources/SpatialAudioBridge/{name}.swift" for name in names]
    sources += [source / "Sources/SpatialAudioPlayback/AtmosOutputActivator.swift", mpv / "audio/out/ao_spatial.swift"]
    silent = source / "Sources/SpatialAudioPlayback/Resources/silent-atmos.mp4"
    resources = output / "SpatialResources.swift"
    resources.write_text('import Foundation\nlet mpSpatialSilentAtmos = Data(base64Encoded: "' +
                         base64.b64encode(silent.read_bytes()).decode() + '")!\n')
    # Since 2.0 the source ships a framework module map for its release
    # framework. The core is linked statically here, so import its header
    # through a plain module of the same name.
    include = output / "include/SpatialAudioCore"
    include.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "Rust/SpatialAudioCore/include/SpatialAudioCore/SpatialAudioCore.h", include)
    (include / "module.modulemap").write_text(
        'module SpatialAudioCore {\n    header "SpatialAudioCore.h"\n    export *\n}\n')
    run("xcrun", "swiftc", "-swift-version", "6", "-O", "-whole-module-optimization",
        "-parse-as-library", "-emit-library", "-static", "-module-name", "MPVSpatial",
        "-file-prefix-map", f"{root}=/mpv-build", "-file-prefix-map", f"{source}=/spatial-audio-bridge",
        "-target", target, "-sdk", sdk, "-I", output / "include",
        *sources, resources, "-o", output / "libmpv_spatial.a", env=environment)


def build(root, source, output, mpv, target, sdk, rust_target, bridge_only=False):
    pin = patches.component_pins("spatialaudiobridge", "apple")
    if not source.exists():
        patches.cmd_fetch("spatialaudiobridge", "apple", str(source))
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin["commit"]:
        raise SystemExit(f"{source}: expected {pin['commit']}, found {revision}")

    series = [root / "patches/spatialaudiobridge/pool" / name
              for name in patches.resolve("spatialaudiobridge", "apple")]
    if series and subprocess.run(["git", "-C", str(source), "apply", "--reverse", "--check",
                                  *map(str, reversed(series))], capture_output=True).returncode:
        if subprocess.run(["git", "-C", str(source), "diff", "--quiet", "HEAD"]).returncode:
            raise SystemExit(f"{source}: source patches differ; preserve edits and use a fresh source checkout")
        patches.cmd_apply("spatialaudiobridge", "apple", str(source), False)

    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, MACOSX_DEPLOYMENT_TARGET="11.0", IPHONEOS_DEPLOYMENT_TARGET="14.0",
                       TVOS_DEPLOYMENT_TARGET="14.0", SDKROOT=str(sdk), ZERO_AR_DATE="1")
    if bridge_only:
        compile_bridge(root, source, output, mpv, target, sdk, environment)
        return

    toolchain = dict(line.split("=", 1) for line in (root / "toolchain/apple.txt").read_text().splitlines()
                     if line and not line.startswith("#"))
    rust = toolchain["rust"]
    # The Apple driver forwards these from its own environment.
    cargo_home = Path(os.environ.get("CARGO_HOME") or Path.home() / ".cargo")
    rustup = cargo_home / "bin/rustup"
    if not rustup.exists():
        rustup = Path(shutil.which("rustup") or "rustup")
    # cargo runs the `rustc` it finds on PATH unless RUSTC names one, and the
    # driver's default PATH (plus the CARGO_HOME/bin rustup prepends) reaches
    # a rustc proxy only when rustup-init populated ~/.cargo/bin. Homebrew's
    # rustup, which macos-26 images ship since 20261021, keeps its proxies in
    # its keg, so name the pinned toolchain's own rustc.
    rustc = subprocess.check_output([str(rustup), "which", "--toolchain", rust, "rustc"], text=True).strip()
    cargo_target = output / "cargo-target"
    manifest = source / "Rust/SpatialAudioCore/Cargo.toml"
    environment.update(CARGO_TARGET_DIR=str(cargo_target), CARGO_PROFILE_RELEASE_LTO="fat", RUSTC=rustc)
    environment.pop("RUSTFLAGS", None)
    sysroot = subprocess.check_output([rustc, "--print", "sysroot"], text=True).strip()
    environment["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join([
        f"--remap-path-prefix={source}=/spatial-audio-bridge",
        f"--remap-path-prefix={cargo_home}=/cargo",
        f"--remap-path-prefix={sysroot}=/rust",
    ])
    try:
        # Building the staticlib alone enables LTO; also emitting an rlib skips
        # it and leaves Rust bitcode that Apple's prelinker cannot consume.
        run(rustup, "run", rust, "cargo", "rustc", "--lib", "--crate-type", "staticlib",
            "--locked", "--release", "-j", "2",
            "--manifest-path", manifest, "--target", rust_target, env=environment)
        prelink_core(cargo_target / rust_target / "release/libspatial_audio_core.a",
                     output, target, sdk, environment)
    finally:
        shutil.rmtree(cargo_target, ignore_errors=True)

    compile_bridge(root, source, output, mpv, target, sdk, environment)

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
    parser.add_argument("--rust-target")
    parser.add_argument("--bridge-only", action="store_true",
                        help="compile only the Swift bridge, as a check without Rust")
    args = parser.parse_args()
    if not args.bridge_only and not args.rust_target:
        parser.error("--rust-target is required unless --bridge-only is given")
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    build(root, args.source.resolve(), args.output.resolve(), args.mpv.resolve(),
          args.target, args.sdk, args.rust_target, args.bridge_only)


if __name__ == "__main__":
    main()
