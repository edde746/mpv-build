import Foundation

do {
    let options = try ArgumentOptions.parse(CommandLine.arguments)
    // Pins live in versions.json at the repository root; load them before
    // performCommand() changes the working directory to dist/.
    try Versions.load()
    try Build.performCommand(options)

    // Dependency order matters: every library links against the ones before it.
    let builds: [BaseBuild] = [
        // libass
        BuildUnibreak(),
        BuildFreetype(),
        BuildFribidi(),
        BuildHarfbuzz(),
        BuildASS(),

        // libbluray
        BuildBluray(),

        // ffmpeg
        BuildOpenSSL(),
        try BuildUavs3d(),
        try BuildDovi(),
        BuildVulkan(),
        try BuildShaderc(),
        BuildLittleCms(),
        BuildPlacebo(),
        BuildDav1d(),
        BuildFFMPEG(),

        // mpv
        BuildUchardet(),
        BuildLuaJIT(),
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

    // for generate Package.swift
    var targets: [PackageTarget] {
        switch self {
        case .libmpv:
            return [
                .target(
                    name: "Libmpv",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libmpv.xcframework.zip",
                    checksum: ""
                )
            ]
        case .FFmpeg:
            return [
                .target(
                    name: "Libavcodec",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libavcodec.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libavdevice",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libavdevice.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libavformat",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libavformat.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libavfilter",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libavfilter.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libavutil",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libavutil.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libswresample",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libswresample.xcframework.zip",
                    checksum: ""
                ),
                .target(
                    name: "Libswscale",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libswscale.xcframework.zip",
                    checksum: ""
                ),
            ]
        case .openssl:
            return [
                .target(
                    name: "Libcrypto",
                    url:
                        "https://github.com/mpvkit/openssl-build/releases/download/\(self.version)/Libcrypto.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/openssl-build/releases/download/\(self.version)/Libcrypto.xcframework.checksum.txt"
                ),
                .target(
                    name: "Libssl",
                    url:
                        "https://github.com/mpvkit/openssl-build/releases/download/\(self.version)/Libssl.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/openssl-build/releases/download/\(self.version)/Libssl.xcframework.checksum.txt"
                ),
            ]
        case .libass:
            return [
                .target(
                    name: "Libass",
                    url:
                        "https://github.com/edde746/mpv-build/releases/download/\(BaseBuild.options.releaseVersion)/Libass.xcframework.zip",
                    checksum: ""
                )
            ]
        case .libunibreak:
            return [
                .target(
                    name: "Libunibreak",
                    url:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libunibreak.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libunibreak.xcframework.checksum.txt"
                )
            ]
        case .libfreetype:
            return [
                .target(
                    name: "Libfreetype",
                    url:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libfreetype.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libfreetype.xcframework.checksum.txt"
                )
            ]
        case .libfribidi:
            return [
                .target(
                    name: "Libfribidi",
                    url:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libfribidi.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libfribidi.xcframework.checksum.txt"
                )
            ]
        case .libharfbuzz:
            return [
                .target(
                    name: "Libharfbuzz",
                    url:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libharfbuzz.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libass-build/releases/download/\(self.version)/Libharfbuzz.xcframework.checksum.txt"
                )
            ]
        case .lcms2:
            return [
                .target(
                    name: "lcms2",
                    url:
                        "https://github.com/mpvkit/lcms2-build/releases/download/\(self.version)/lcms2.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/lcms2-build/releases/download/\(self.version)/lcms2.xcframework.checksum.txt"
                )
            ]
        case .libplacebo:
            return [
                .target(
                    name: "Libplacebo",
                    url:
                        "https://github.com/mpvkit/libplacebo-build/releases/download/\(self.version)/Libplacebo.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libplacebo-build/releases/download/\(self.version)/Libplacebo.xcframework.checksum.txt"
                )
            ]
        case .libdav1d:
            return [
                .target(
                    name: "Libdav1d",
                    url:
                        "https://github.com/edde746/libdav1d-build/releases/download/\(self.version)/Libdav1d.xcframework.zip",
                    checksum:
                        "https://github.com/edde746/libdav1d-build/releases/download/\(self.version)/Libdav1d.xcframework.checksum.txt"
                )
            ]
        case .libdovi:
            return [
                .target(
                    name: "Libdovi",
                    url:
                        "https://github.com/mpvkit/libdovi-build/releases/download/\(self.version)/Libdovi.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libdovi-build/releases/download/\(self.version)/Libdovi.xcframework.checksum.txt"
                )
            ]
        case .vulkan:
            return [
                .target(
                    name: "MoltenVK",
                    url:
                        "https://github.com/mpvkit/moltenvk-build/releases/download/\(self.version)/MoltenVK.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/moltenvk-build/releases/download/\(self.version)/MoltenVK.xcframework.checksum.txt"
                )
            ]
        case .libshaderc:
            return [
                .target(
                    name: "Libshaderc_combined",
                    url:
                        "https://github.com/mpvkit/libshaderc-build/releases/download/\(self.version)/Libshaderc_combined.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libshaderc-build/releases/download/\(self.version)/Libshaderc_combined.xcframework.checksum.txt"
                )
            ]
        case .libuchardet:
            return [
                .target(
                    name: "Libuchardet",
                    url:
                        "https://github.com/mpvkit/libuchardet-build/releases/download/\(self.version)/Libuchardet.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libuchardet-build/releases/download/\(self.version)/Libuchardet.xcframework.checksum.txt"
                )
            ]
        case .libbluray:
            return [
                .target(
                    name: "Libbluray",
                    url:
                        "https://github.com/mpvkit/libbluray-build/releases/download/\(self.version)/Libbluray.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libbluray-build/releases/download/\(self.version)/Libbluray.xcframework.checksum.txt"
                )
            ]
        case .libluajit:
            return [
                .target(
                    name: "Libluajit",
                    url:
                        "https://github.com/mpvkit/libluajit-build/releases/download/\(self.version)/Libluajit.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libluajit-build/releases/download/\(self.version)/Libluajit.xcframework.checksum.txt"
                )
            ]
        case .libuavs3d:
            return [
                .target(
                    name: "Libuavs3d",
                    url:
                        "https://github.com/mpvkit/libuavs3d-build/releases/download/\(self.version)/Libuavs3d.xcframework.zip",
                    checksum:
                        "https://github.com/mpvkit/libuavs3d-build/releases/download/\(self.version)/Libuavs3d.xcframework.checksum.txt"
                )
            ]
        }
    }
}

private class BuildMPV: BaseBuild {
    init() {
        super.init(library: .libmpv)
    }

    override func flagsDependencelibrarys() -> [Library] {
        []
    }

    override func arguments(platform: PlatformType, arch: ArchType) -> [String] {
        var array = [
            "-Dlibmpv=true",
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
        let dependencyLibrary = [
            Library.libfreetype, .libharfbuzz, .libfribidi, .libass, .vulkan,
            .libshaderc, .lcms2, .libplacebo, .libdav1d, .libuavs3d, .openssl,
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

private class BuildOpenSSL: ZipBaseBuild {
    init() {
        super.init(library: .openssl)
    }
}

private class BuildBluray: ZipBaseBuild {
    init() {
        super.init(library: .libbluray)
    }
}

private class BuildUchardet: ZipBaseBuild {
    init() {
        super.init(library: .libuchardet)
    }
}

private class BuildLuaJIT: ZipBaseBuild {
    init() {
        super.init(library: .libluajit)
    }
}

private class BuildPlacebo: ZipBaseBuild {
    init() {
        super.init(library: .libplacebo)
    }
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

private class BuildUnibreak: ZipBaseBuild {
    init() {
        super.init(library: .libunibreak)
    }
}

private class BuildFreetype: ZipBaseBuild {
    init() {
        super.init(library: .libfreetype)
    }
}

private class BuildFribidi: ZipBaseBuild {
    init() {
        super.init(library: .libfribidi)
    }
}

private class BuildHarfbuzz: ZipBaseBuild {
    init() {
        super.init(library: .libharfbuzz)
    }
}

private class BuildDav1d: ZipBaseBuild {
    init() {
        super.init(library: .libdav1d)
    }

    override func buildALL() throws {
        try super.buildALL()

        // // TODO: maccatalyst平台会导致ffmpeg编译失败，暂时删除忽略
        // if platform == .maccatalyst {
        //     return
        // }
    }
}

private class BuildLittleCms: ZipBaseBuild {
    init() {
        super.init(library: .lcms2)
    }
}

private class BuildUavs3d: ZipBaseBuild {
    init() throws {
        super.init(library: .libuavs3d)
    }
}

private class BuildDovi: ZipBaseBuild {
    init() throws {
        super.init(library: .libdovi)
    }
}

private class BuildShaderc: ZipBaseBuild {
    init() throws {
        super.init(library: .libshaderc)
    }
}

private class BuildVulkan: ZipBaseBuild {
    init() {
        super.init(library: .vulkan)
    }

    override func buildALL() throws {
        try super.beforeBuild()

        try? FileManager.default.removeItem(at: URL.currentDirectory + library.rawValue)
        try? FileManager.default.removeItem(at: directoryURL.appendingPathExtension("log"))
        try? FileManager.default.createDirectory(
            atPath: (URL.currentDirectory + library.rawValue).path,
            withIntermediateDirectories: true, attributes: nil)
        for platform in BaseBuild.platforms {
            for arch in architectures(platform) {
                // restore lib
                let srcThinLibPath =
                    directoryURL + ["lib", "MoltenVK.xcframework", platform.frameworkName]
                let destThinPath = thinDir(platform: platform, arch: arch)
                let destThinLibPath = destThinPath + ["lib"]
                try? FileManager.default.createDirectory(
                    atPath: destThinPath.path, withIntermediateDirectories: true, attributes: nil)
                try? FileManager.default.copyItem(at: srcThinLibPath, to: destThinLibPath)

                // restore include
                let srcIncludePath = directoryURL + ["include"]
                let destIncludePath = destThinPath + ["include"]
                try? FileManager.default.copyItem(at: srcIncludePath, to: destIncludePath)

                // restore pkgconfig
                let srcPkgConfigPath =
                    directoryURL + ["pkgconfig-example", platform.rawValue, arch.rawValue]
                let destPkgConfigPath = destThinPath + ["lib", "pkgconfig"]
                try? FileManager.default.copyItem(at: srcPkgConfigPath, to: destPkgConfigPath)
                Utility.listAllFiles(in: destPkgConfigPath).forEach { file in
                    if let data = FileManager.default.contents(atPath: file.path),
                        var str = String(data: data, encoding: .utf8)
                    {
                        str = str.replacingOccurrences(
                            of: "/path/to/workdir", with: URL.currentDirectory.path)
                        try! str.write(toFile: file.path, atomically: true, encoding: .utf8)
                    }
                }
            }
        }

        try super.afterBuild()
    }
}
