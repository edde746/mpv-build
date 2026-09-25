import Foundation

do {
    let options = try ArgumentOptions.parse(CommandLine.arguments)
    // Pins live in versions.json at the repository root; load them before
    // performCommand() changes the working directory to dist/.
    try Versions.load()
    try Build.performCommand(options)

    // Dependency order matters: every library links against the ones before it.
    // `ZipBaseBuild` is a concrete class whose only parameter is the library, so
    // the prebuilt-download libraries need no named subclass.
    let builds: [BaseBuild] = [
        // libass
        ZipBaseBuild(library: .libunibreak),
        ZipBaseBuild(library: .libfreetype),
        ZipBaseBuild(library: .libfribidi),
        ZipBaseBuild(library: .libharfbuzz),
        BuildASS(),

        // libbluray
        ZipBaseBuild(library: .libbluray),

        // ffmpeg
        ZipBaseBuild(library: .openssl),
        ZipBaseBuild(library: .libuavs3d),
        ZipBaseBuild(library: .libdovi),
        BuildVulkan(),
        ZipBaseBuild(library: .libshaderc),
        ZipBaseBuild(library: .lcms2),
        ZipBaseBuild(library: .libplacebo),
        ZipBaseBuild(library: .libdav1d),
        BuildFFMPEG(),

        // mpv
        ZipBaseBuild(library: .libuchardet),
        ZipBaseBuild(library: .libluajit),
        BuildMPV(),
    ]

    if !options.libs.isEmpty {
        print("libs=\(options.libs.map(\.rawValue).joined(separator: ",")): only these libraries are compiled from source")
    }
    if options.usePrebuilt {
        print("use-prebuilt: self-built libraries outside libs= are restored from artifacts.json when it covers them")
    }

    // No library is ever skipped: `buildALL()` restores it from the manifest when that is
    // possible and compiles it otherwise, so an unlisted, unpublished library still builds.
    for build in builds {
        try build.buildALL()
    }
} catch {
    print(error.localizedDescription)
    exit(1)
}

/// One component entry of `versions.json`. Unknown keys (platforms, overrides,
/// provenance, ...) are ignored here: the Swift driver only consumes the pins
/// it clones or downloads from.
struct ComponentPin: Decodable {
    let kind: String
    let version: String
    let url: String
    let sha256: String?
    let ref: String?
    let commit: String?
}

/// Reader for the repository-root `versions.json`, the single source of truth
/// for every component's version and acquisition URL across all platform
/// drivers. Loaded once at tool startup, while the working directory is still
/// the repository root.
enum Versions {
    private struct Root: Decodable {
        let formatVersion: Int
        let components: [String: ComponentPin]
    }

    private static var components: [String: ComponentPin] = [:]

    static func load(from url: URL = URL.currentDirectory + "versions.json") throws {
        let data = try Data(contentsOf: url)
        components = try JSONDecoder().decode(Root.self, from: data).components
    }

    static func pin(_ name: String) -> ComponentPin {
        guard let pin = components[name] else {
            fatalError("versions.json has no component named \(name)")
        }
        return pin
    }
}

enum Library: String, CaseIterable {
    case libmpv, FFmpeg, libshaderc, vulkan, lcms2, libdovi, openssl, libunibreak, libfreetype,
        libfribidi, libharfbuzz, libass, libplacebo, libdav1d, libuchardet, libbluray, libluajit, libuavs3d

    /// Component name in `versions.json` and `patches/`. The only mapping
    /// between Swift raw values and canonical component names.
    var canonicalName: String {
        switch self {
        case .libmpv:
            return "mpv"
        case .FFmpeg:
            return "ffmpeg"
        default:
            return rawValue.lowercased()
        }
    }

    var version: String {
        Versions.pin(canonicalName).version
    }

    var url: String {
        Versions.pin(canonicalName).url
    }

    /// The xcframework names this library publishes, in the order `Package.swift`
    /// lists them. `artifacts.json` keys its framework entries by these names, and
    /// a prebuilt restore is only taken when every one of them is present.
    var frameworks: [String] {
        switch self {
        case .libmpv:
            return ["Libmpv"]
        case .FFmpeg:
            return [
                "Libavcodec", "Libavdevice", "Libavformat", "Libavfilter", "Libavutil",
                "Libswresample", "Libswscale",
            ]
        case .openssl:
            return ["Libcrypto", "Libssl"]
        case .libass:
            return ["Libass"]
        case .libunibreak:
            return ["Libunibreak"]
        case .libfreetype:
            return ["Libfreetype"]
        case .libfribidi:
            return ["Libfribidi"]
        case .libharfbuzz:
            return ["Libharfbuzz"]
        case .lcms2:
            return ["lcms2"]
        case .libplacebo:
            return ["Libplacebo"]
        case .libdav1d:
            return ["Libdav1d"]
        case .libdovi:
            return ["Libdovi"]
        case .vulkan:
            return ["MoltenVK"]
        case .libshaderc:
            return ["Libshaderc_combined"]
        case .libuchardet:
            return ["Libuchardet"]
        case .libbluray:
            return ["Libbluray"]
        case .libluajit:
            return ["Libluajit"]
        case .libuavs3d:
            return ["Libuavs3d"]
        }
    }
}

private class BuildMPV: BaseBuild {
    init() {
        super.init(library: .libmpv)
    }

    override func arguments(platform: PlatformType, arch: ArchType) -> [String] {
        var array = [
            "-Dlibmpv=true",
            // No __DATE__/__TIME__ stamp: a rebuild of a published content key
            // has to reproduce its bytes (keys.py publish-assets checks). The
            // android and linux drivers pass the same flag.
            "-Dbuild-date=false",
            "-Dgl=enabled",
            "-Dplain-gl=enabled",
            "-Diconv=enabled",
            "-Duchardet=enabled",
            "-Dvulkan=enabled",
            "-Dmoltenvk=enabled",  // from patch option

            "-Djavascript=disabled",
            "-Dzimg=disabled",
            "-Djpeg=disabled",
            "-Dvapoursynth=disabled",
            "-Drubberband=disabled",
        ]
        array.append("-Dgpl=true")
        let blurayLibPath =
            URL.currentDirectory + [
                Library.libbluray.rawValue, platform.rawValue, "thin", arch.rawValue,
            ]
        if FileManager.default.fileExists(atPath: blurayLibPath.path) {
            array.append("-Dlibbluray=enabled")
        } else {
            array.append("-Dlibbluray=disabled")
        }
        if !(platform == .macos && arch.executable) {
            array.append("-Dcplayer=false")
        }
        if platform == .macos {
            array.append(
                "-Dswift-flags=-sdk \(platform.isysroot) -target \(platform.deploymentTarget(arch))"
            )
            array.append("-Dcocoa=enabled")
            array.append("-Dcoreaudio=enabled")
            array.append("-Davfoundation=enabled")
            array.append("-Dvo-avfoundation=enabled")
            array.append("-Dgl-cocoa=enabled")
            array.append("-Dvideotoolbox-gl=enabled")
            array.append("-Dvideotoolbox-pl=enabled")
            array.append("-Dmacos-touchbar=disabled")
            array.append("-Dmacos-media-player=disabled")
            array.append("-Dlua=luajit")
        } else {
            array.append("-Dvideotoolbox-gl=disabled")
            array.append("-Dvideotoolbox-pl=enabled")
            array.append("-Dswift-build=disabled")
            array.append("-Daudiounit=enabled")
            array.append("-Davfoundation=enabled")
            array.append("-Dvo-avfoundation=enabled")
            array.append("-Dlua=disabled")
            if platform == .maccatalyst {
                array.append("-Dcocoa=disabled")
                array.append("-Dcoreaudio=disabled")
            } else {
                array.append("-Dios-gl=enabled")
            }
        }
        return array
    }

}

private class BuildFFMPEG: BaseBuild {
    init() {
        super.init(library: .FFmpeg)
    }

    override func beforeBuild() throws {
        try super.beforeBuild()

        if Utility.shell("which nasm") == nil {
            Utility.shell("brew install nasm")
        }
        if Utility.shell("which sdl2-config") == nil {
            Utility.shell("brew install sdl2")
        }

        let lldbFile = URL.currentDirectory + "LLDBInitFile"
        try? FileManager.default.removeItem(at: lldbFile)
        FileManager.default.createFile(atPath: lldbFile.path, contents: nil, attributes: nil)
        let path = directoryURL + "libavcodec/videotoolbox.c"
        if let data = FileManager.default.contents(atPath: path.path), var str = String(data: data, encoding: .utf8) {
            str = str.replacingOccurrences(of: "kCVPixelBufferOpenGLESCompatibilityKey", with: "kCVPixelBufferMetalCompatibilityKey")
            str = str.replacingOccurrences(of: "kCVPixelBufferIOSurfaceOpenGLTextureCompatibilityKey", with: "kCVPixelBufferMetalCompatibilityKey")
            try? str.write(toFile: path.path, atomically: true, encoding: .utf8)
        }
    }

    override func flagsDependencelibrarys() -> [Library] {
        [.libdovi]
    }

    override func frameworks() throws -> [String] {
        var frameworks: [String] = []
        if let platform = platforms().first {
            if let arch = platform.architectures.first {
                let lib = thinDir(platform: platform, arch: arch) + "lib"
                let fileNames = try FileManager.default.contentsOfDirectory(atPath: lib.path)
                for fileName in fileNames {
                    if fileName.hasPrefix("lib"), fileName.hasSuffix(".a") {
                        // 因为其他库也可能引入libavformat,所以把lib改成大写，这样就可以排在前面，覆盖别的库。
                        frameworks.append("Lib" + fileName.dropFirst(3).dropLast(2))
                    }
                }
            }
        }
        return frameworks
    }

    override func build(platform: PlatformType, arch: ArchType) throws {
        try super.build(platform: platform, arch: arch)
        let buildURL = scratch(platform: platform, arch: arch)
        let prefix = thinDir(platform: platform, arch: arch)
        let lldbFile = URL.currentDirectory + "LLDBInitFile"
        if let data = FileManager.default.contents(atPath: lldbFile.path),
            var str = String(data: data, encoding: .utf8)
        {
            str.append(
                "settings \(str.isEmpty ? "set" : "append") target.source-map \((buildURL + "src").path) \(directoryURL.path)\n"
            )
            try str.write(toFile: lldbFile.path, atomically: true, encoding: .utf8)
        }
        try FileManager.default.copyItem(
            at: buildURL + "config.h", to: prefix + "include/libavutil/config.h")
        try FileManager.default.copyItem(
            at: buildURL + "config.h", to: prefix + "include/libavcodec/config.h")
        try FileManager.default.copyItem(
            at: buildURL + "config.h", to: prefix + "include/libavformat/config.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/getenv_utf8.h",
            to: prefix + "include/libavutil/getenv_utf8.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/libm.h", to: prefix + "include/libavutil/libm.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/thread.h", to: prefix + "include/libavutil/thread.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/intmath.h", to: prefix + "include/libavutil/intmath.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/mem_internal.h",
            to: prefix + "include/libavutil/mem_internal.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/attributes_internal.h",
            to: prefix + "include/libavutil/attributes_internal.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavcodec/mathops.h", to: prefix + "include/libavcodec/mathops.h")
        try FileManager.default.copyItem(
            at: buildURL + "src/libavformat/os_support.h",
            to: prefix + "include/libavformat/os_support.h")
        let internalPath = prefix + "include/libavutil/internal.h"
        try FileManager.default.copyItem(
            at: buildURL + "src/libavutil/internal.h", to: internalPath)
        if let data = FileManager.default.contents(atPath: internalPath.path),
            var str = String(data: data, encoding: .utf8)
        {
            str = str.replacingOccurrences(
                of: """
                    #include "timer.h"
                    """,
                with: """
                    // #include "timer.h"
                    """)
            str = str.replacingOccurrences(
                of: "kCVPixelBufferIOSurfaceOpenGLTextureCompatibilityKey",
                with: "kCVPixelBufferMetalCompatibilityKey")
            try str.write(toFile: internalPath.path, atomically: true, encoding: .utf8)
        }

    }

    override func arguments(platform: PlatformType, arch: ArchType) -> [String] {
        var arguments = ffmpegConfiguers
        if BaseBuild.options.enableDebug {
            arguments.append("--enable-debug")
            arguments.append("--disable-stripping")
            arguments.append("--disable-optimizations")
        } else {
            arguments.append("--disable-debug")
            arguments.append("--enable-stripping")
            arguments.append("--enable-optimizations")
        }
        arguments.append("--enable-gpl")
        // arguments += Build.ffmpegConfiguers
        arguments.append("--disable-large-tests")
        arguments.append("--ignore-tests=TESTS")
        arguments.append("--arch=\(arch.cpuFamily)")
        arguments.append("--target-os=darwin")
        // arguments.append(arch.cpu())

        /**
         aacpsdsp.o), building for Mac Catalyst, but linking in object file built for
         x86_64 binaries are built without ASM support, since ASM for x86_64 is actually x86 and that confuses `xcodebuild -create-xcframework` https://stackoverflow.com/questions/58796267/building-for-macos-but-linking-in-object-file-built-for-free-standing/59103419#59103419
         */
        if platform == .maccatalyst || arch == .x86_64 {
            arguments.append("--disable-neon")
            arguments.append("--disable-asm")
        } else {
            arguments.append("--enable-neon")
            arguments.append("--enable-asm")
        }
        if platform == .macos, arch.executable {
            arguments.append("--enable-ffplay")
            arguments.append("--enable-sdl2")
            arguments.append("--enable-decoder=rawvideo")
            arguments.append("--enable-filter=color")
            arguments.append("--enable-filter=lut")
            arguments.append("--enable-filter=testsrc")
        } else {
            arguments.append("--disable-programs")
        }
        //        if platform == .isimulator || platform == .tvsimulator {
        //            arguments.append("--assert-level=1")
        //        }
        // libdovi is what makes Dolby Vision profile 7 conversion possible:
        // patch 0021 registers it with configure and #if CONFIG_LIBDOVI guards
        // the real conversion, so without --enable-libdovi the VideoToolbox
        // path keeps asking for profile 8.1 and always gets ENOSYS back.
        let dependencyLibrary = [
            Library.libfreetype, .libharfbuzz, .libfribidi, .libass, .vulkan,
            .libshaderc, .lcms2, .libplacebo, .libdav1d, .libdovi, .libuavs3d,
            .openssl,
        ]
        for library in dependencyLibrary {
            let path =
                URL.currentDirectory + [library.rawValue, platform.rawValue, "thin", arch.rawValue]
            if FileManager.default.fileExists(atPath: path.path) {
                arguments.append("--enable-\(library.rawValue)")
                if library == .libdav1d || library == .libuavs3d {
                    arguments.append("--enable-decoder=\(library.rawValue)")
                } else if library == .libass {
                    arguments.append("--enable-filter=ass")
                    arguments.append("--enable-filter=subtitles")
                } else if library == .libplacebo {
                    arguments.append("--enable-filter=libplacebo")
                }
            }
        }

        return arguments
    }

    override func frameworkExcludeHeaders(_ framework: String) -> [String] {
        if framework == "Libavcodec" {
            return ["xvmc", "vdpau", "qsv", "dxva2", "d3d11va", "d3d12va"]
        } else if framework == "Libavutil" {
            return [
                "hwcontext_vulkan", "hwcontext_vdpau", "hwcontext_vaapi", "hwcontext_qsv",
                "hwcontext_opencl", "hwcontext_dxva2", "hwcontext_d3d11va", "hwcontext_d3d12va",
                "hwcontext_cuda",
            ]
        } else {
            return super.frameworkExcludeHeaders(framework)
        }
    }

    private let ffmpegConfiguers = [
        // Configuration options:
        "--disable-armv5te", "--disable-armv6", "--disable-armv6t2",
        "--disable-bzlib", "--disable-gray", "--disable-iconv", "--disable-linux-perf",
        "--disable-shared", "--disable-small", "--disable-symver", "--disable-xlib",
        "--enable-cross-compile", "--enable-libxml2",
        "--enable-optimizations", "--enable-pic", "--enable-runtime-cpudetect", "--enable-static",
        "--enable-thumb", "--enable-version3",
        "--pkg-config-flags=--static",
        // Documentation options:
        "--disable-doc", "--disable-htmlpages", "--disable-manpages", "--disable-podpages",
        "--disable-txtpages",
        // Component options:
        "--enable-avcodec", "--enable-avformat", "--enable-avutil", "--enable-network",
        "--enable-swresample", "--enable-swscale",
        "--disable-securetransport", "--disable-gnutls",
        "--disable-libtls", "--disable-mbedtls",
        "--disable-devices", "--disable-outdevs", "--disable-indevs",
        // ,"--disable-pthreads"
        // ,"--disable-w32threads"
        // ,"--disable-os2threads"
        // ,"--disable-dct"
        // ,"--disable-dwt"
        // ,"--disable-lsp"
        // ,"--disable-lzo"
        // ,"--disable-mdct"
        // ,"--disable-rdft"
        // ,"--disable-fft"
        // Hardware accelerators:
        "--disable-d3d11va", "--disable-d3d12va", "--disable-dxva2", "--disable-vaapi",
        "--disable-vdpau",
        // Individual component options:
        // ,"--disable-everything"
        // ./configure --list-muxers
        "--enable-muxers",
        // ./configure --list-encoders
        "--enable-encoders",
        // ./configure --list-protocols
        "--enable-protocols",
        // ./configure --list-demuxers
        "--enable-demuxers",
        // ./configure --list-bsfs
        "--enable-bsfs",
        // ./configure --list-decoders
        "--enable-decoders",

        // ./configure --list-filters
        "--disable-filters",
        "--enable-filter=crop",
        "--enable-filter=aformat", "--enable-filter=amix", "--enable-filter=anull",
        "--enable-filter=aresample",
        "--enable-filter=areverse", "--enable-filter=asetrate", "--enable-filter=atempo",
        "--enable-filter=atrim",
        "--enable-filter=bwdif", "--enable-filter=delogo",
        "--enable-filter=equalizer", "--enable-filter=estdif",
        "--enable-filter=firequalizer", "--enable-filter=format", "--enable-filter=fps",
        "--enable-filter=hflip", "--enable-filter=hwdownload", "--enable-filter=hwmap",
        "--enable-filter=hwupload",
        "--enable-filter=idet", "--enable-filter=lenscorrection", "--enable-filter=lut*",
        "--enable-filter=negate", "--enable-filter=null",
        "--enable-filter=overlay",
        "--enable-filter=palettegen", "--enable-filter=paletteuse", "--enable-filter=pan",
        "--enable-filter=rotate",
        "--enable-filter=scale", "--enable-filter=setpts", "--enable-filter=superequalizer",
        "--enable-filter=transpose", "--enable-filter=trim",
        "--enable-filter=vflip", "--enable-filter=volume", "--enable-filter=loudnorm",
        "--enable-filter=w3fdif",
        "--enable-filter=yadif",
        "--enable-filter=avgblur_vulkan", "--enable-filter=blend_vulkan",
        "--enable-filter=bwdif_vulkan",
        "--enable-filter=chromaber_vulkan", "--enable-filter=flip_vulkan",
        "--enable-filter=gblur_vulkan",
        "--enable-filter=hflip_vulkan", "--enable-filter=nlmeans_vulkan",
        "--enable-filter=overlay_vulkan",
        "--enable-filter=vflip_vulkan", "--enable-filter=xfade_vulkan",
    ]

}

private class BuildASS: BaseBuild {
    init() {
        super.init(library: .libass)
    }

    override func arguments(platform: PlatformType, arch: ArchType) -> [String] {
        // AArch64 asm assembles with clang; x86 asm would need nasm, so keep
        // SIMD to the arm64 slices (mirrors the FFmpeg asm policy).
        let asm = arch == .arm64 || arch == .arm64e
        return [
            "-Dasm=\(asm ? "enabled" : "disabled")",
            "-Dlibunibreak=enabled",
            "-Dcoretext=enabled",
            "-Dfontconfig=disabled",
            "-Ddirectwrite=disabled",
            "-Dcheckasm=disabled",
            "-Dtest=disabled",
            "-Dprofile=disabled",
            "-Dcompare=disabled",
            "-Dfuzz=disabled",
        ]
    }
}

private class BuildVulkan: ZipBaseBuild {
    init() {
        super.init(library: .vulkan)
    }

    /// MoltenVK's `-all.zip` carries the static library inside the xcframework
    /// slice rather than in the shared `lib/<platform>/thin/<arch>/lib` layout.
    override func unpackedThinLib(root: URL, platform: PlatformType, arch: ArchType) -> URL {
        root + ["lib", "MoltenVK.xcframework", platform.frameworkName]
    }
}
