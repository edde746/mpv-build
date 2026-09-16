import Foundation

enum Build {
    static func performCommand(_ options: ArgumentOptions) throws {
        if Utility.shell("which brew") == nil {
            print("""
            You need to run the script first
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
            """)
            return
        }
        if Utility.shell("which pkg-config") == nil {
            Utility.shell("brew install pkg-config")
        }
        if Utility.shell("which wget") == nil {
            Utility.shell("brew install wget")
        }
        let path = URL.currentDirectory + "dist"
        if !FileManager.default.fileExists(atPath: path.path) {
            try? FileManager.default.createDirectory(at: path, withIntermediateDirectories: false, attributes: nil)
        }
        FileManager.default.changeCurrentDirectoryPath(path.path)
        BaseBuild.options = options
        if !options.platforms.isEmpty {
            BaseBuild.platforms = options.platforms
        }
    }
}


class ArgumentOptions {
    var enableDebug: Bool = false
    var platforms : [PlatformType] = []
    /// Libraries that must be compiled from source. Empty means "no restriction".
    var libs: [Library] = []
    /// Restore every self-built library that is not in `libs` from its published prebuilt zips.
    var usePrebuilt: Bool = false

    static func parse(_ arguments: [String]) throws -> ArgumentOptions {
        let options = ArgumentOptions()
        func appendPlatform(_ platform: PlatformType) {
            if !options.platforms.contains(platform) {
                options.platforms += [platform]
            }
        }

        for argument in arguments {
            switch argument {
            case "enable-debug":
                options.enableDebug = true
            case "use-prebuilt":
                options.usePrebuilt = true
            default:
                if argument.hasPrefix("platform=") {
                    let values = String(argument.suffix(argument.count - "platform=".count))
                    for val in values.split(separator: ",") {
                        let platformStr = val.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                        switch platformStr {
                        case "ios":
                            appendPlatform(.ios)
                            appendPlatform(.isimulator)
                        case "ios-device", "iphoneos":
                            appendPlatform(.ios)
                        case "ios-simulator", "iossim", "isimulator":
                            appendPlatform(.isimulator)
                        case "tvos":
                            appendPlatform(.tvos)
                            appendPlatform(.tvsimulator)
                        case "tvos-device":
                            appendPlatform(.tvos)
                        case "tvos-simulator", "tvossim", "tvsimulator":
                            appendPlatform(.tvsimulator)
                        default:
                            guard let other = PlatformType(rawValue: platformStr) else { throw NSError(domain: "unknown platform: \(val)", code: 1) }
                            appendPlatform(other)
                        }
                    }
                }
                if argument.hasPrefix("libs=") {
                    let values = String(argument.suffix(argument.count - "libs=".count))
                    for val in values.split(separator: ",") {
                        let name = val.trimmingCharacters(in: .whitespacesAndNewlines)
                        if name.isEmpty {
                            continue
                        }
                        guard let lib = Library.allCases.first(where: { $0.rawValue.lowercased() == name.lowercased() }) else {
                            throw NSError(domain: "unknown library: \(val)", code: 1)
                        }
                        if !options.libs.contains(lib) {
                            options.libs += [lib]
                        }
                    }
                }
            }
        }

        return options
    }
}

/// Reader for the apple section of the repo-root `artifacts.json`, the committed
/// record of the content-addressed binaries published for the self-built
/// libraries. The file is written by `scripts/keys.py`; a missing file or a
/// missing library entry is a soft miss so that a fresh checkout can still
/// build everything from source.
final class BinaryManifest {
    struct Entry {
        let key: String
        let prebuilt: [PlatformType: String]
        /// Names of the frameworks this library publishes. The manifest on disk
        /// nests an `{asset, checksum}` record per framework, but a prebuilt
        /// restore only needs to know which names the entry carries (the zips
        /// already contain the binary), so only the names are kept here.
        let frameworks: Set<String>
    }

    let assetBase: String
    private let entries: [String: Entry]

    private static var didLoad = false
    private static var loaded: BinaryManifest?

    /// Lazily parsed manifest, or nil when it is absent or unreadable.
    static var shared: BinaryManifest? {
        if !didLoad {
            didLoad = true
            loaded = BinaryManifest(contentsOf: URL.currentDirectory + ["..", "artifacts.json"])
        }
        return loaded
    }

    init?(contentsOf url: URL) {
        guard let data = FileManager.default.contents(atPath: url.path),
              let manifest = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let root = (manifest["platforms"] as? [String: Any])?["apple"] as? [String: Any],
              let assetBase = root["assetBase"] as? String
        else {
            return nil
        }
        self.assetBase = assetBase

        var entries: [String: Entry] = [:]
        for (name, value) in root["libraries"] as? [String: Any] ?? [:] {
            guard let library = value as? [String: Any], let key = library["key"] as? String else {
                continue
            }
            var prebuilt: [PlatformType: String] = [:]
            for (platformName, asset) in library["prebuilt"] as? [String: String] ?? [:] {
                guard let platform = PlatformType(rawValue: platformName) else {
                    continue
                }
                prebuilt[platform] = asset
            }
            var frameworks: Set<String> = []
            for (frameworkName, value) in library["frameworks"] as? [String: Any] ?? [:] {
                // The published shape is validated even though only the name is
                // kept: an incomplete entry must still read as missing so the
                // library falls back to a source build.
                guard let framework = value as? [String: Any],
                      framework["asset"] is String,
                      framework["checksum"] is String
                else {
                    continue
                }
                frameworks.insert(frameworkName)
            }
            entries[name] = Entry(key: key, prebuilt: prebuilt, frameworks: frameworks)
        }
        self.entries = entries
    }

    func entry(for library: Library) -> Entry? {
        entries[library.rawValue]
    }

    func url(ofAsset asset: String) -> String {
        "\(assetBase)/\(asset)"
    }
}

class BaseBuild {
    static let defaultPath = "/Library/Frameworks/Python.framework/Versions/Current/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
    static var platforms = PlatformType.allCases.filter { $0 != .xros && $0 != .xrsimulator }
    static var options = ArgumentOptions()
    let library: Library
    let directoryURL: URL
    let xcframeworkDirectoryURL: URL
    init(library: Library) {
        self.library = library
        directoryURL = URL.currentDirectory + "\(library.rawValue)-\(library.version)"
        xcframeworkDirectoryURL = URL.currentDirectory + ["release", "xcframework"]
    }

    func beforeBuild() throws {
        let patchFiles = try patchFileURLs()
        let expectedPatchState = patchFiles.isEmpty ? nil : try makePatchState(patchFiles: patchFiles)
        let patchStateFile = patchStateURL()

        if FileManager.default.fileExists(atPath: directoryURL.path) {
            guard let expectedPatchState else {
                return
            }

            let currentPatchState = try? String(contentsOf: patchStateFile, encoding: .utf8)
            if currentPatchState == expectedPatchState {
                return
            }

            print("Patch cache changed for \(library.rawValue); refreshing source checkout")
            try FileManager.default.removeItem(at: directoryURL)
            try? FileManager.default.removeItem(at: patchStateFile)
        }

        // pull code from git
        try! Utility.launch(path: "/usr/bin/git", arguments: ["-c", "advice.detachedHead=false", "clone", "--recursive", "--depth", "1", "--branch", library.version, library.url, directoryURL.path])

        for patchFile in patchFiles {
            try! Utility.launch(path: "/usr/bin/git", arguments: ["apply", patchFile.path], currentDirectoryURL: directoryURL)
        }

        if let expectedPatchState {
            try expectedPatchState.write(to: patchStateFile, atomically: true, encoding: .utf8)
        }
    }

    private func patchesRootURL() -> URL {
        URL.currentDirectory + "../patches/\(library.canonicalName)"
    }

    private func patchStateURL() -> URL {
        directoryURL.appendingPathExtension("patch-state")
    }

    /// Resolved patch series for the apple platform group: the entries of
    /// series.common followed by the entries of series.apple, each naming a
    /// file in pool/. Series order is authoritative -- nothing is sorted. A
    /// missing series file is an empty series; `#` comments and blank lines
    /// are ignored. Mirrors `scripts/patches.py resolve <component> apple`.
    private func patchFileURLs() throws -> [URL] {
        let root = patchesRootURL()
        var urls: [URL] = []
        for series in ["series.common", "series.apple"] {
            guard let data = FileManager.default.contents(atPath: (root + series).path),
                  let text = String(data: data, encoding: .utf8)
            else {
                continue
            }
            for rawLine in text.split(separator: "\n") {
                let line = rawLine.trimmingCharacters(in: .whitespaces)
                if line.isEmpty || line.hasPrefix("#") {
                    continue
                }
                urls.append(root + ["pool", line])
            }
        }
        return urls
    }

    private func makePatchState(patchFiles: [URL]) throws -> String {
        var state = ""
        state += "library=\(library.rawValue)\n"
        state += "version=\(library.version)\n"
        state += "url=\(library.url)\n"

        for patchFile in patchFiles {
            let data = try Data(contentsOf: patchFile)
            state += "patch=\(patchFile.lastPathComponent)\n"
            state += "bytes=\(data.count)\n"
            state += data.base64EncodedString(options: [.lineLength64Characters])
            state += "\n"
        }

        return state
    }

    func buildALL() throws {
        if let source = prebuiltSource() {
            try restoreFromPrebuilt(entry: source.entry, manifest: source.manifest)
            return
        }
        print("Build \(library.rawValue) \(library.version) from source")
        try beforeBuild()
        try? FileManager.default.removeItem(at: URL.currentDirectory + library.rawValue)
        try? FileManager.default.removeItem(at: directoryURL.appendingPathExtension("log"))
        for platform in BaseBuild.platforms {
            for arch in architectures(platform) {
                try build(platform: platform, arch: arch)
            }
        }
        try createXCFramework()
        try packageRelease()
    }

    /// Manifest entry to restore this library from, or nil when it has to be compiled.
    /// A library is only restored when `use-prebuilt` was requested, it was not named in
    /// `libs=`, and the manifest carries every platform zip plus every framework this
    /// library publishes. Anything less falls back to compiling: correctness over speed.
    func prebuiltSource() -> (manifest: BinaryManifest, entry: BinaryManifest.Entry)? {
        guard BaseBuild.options.usePrebuilt else {
            return nil
        }
        if BaseBuild.options.libs.contains(library) {
            return nil
        }
        guard let manifest = BinaryManifest.shared, let entry = manifest.entry(for: library) else {
            print("No prebuilt entry for \(library.rawValue) in artifacts.json; building from source")
            return nil
        }
        let missingPlatforms = BaseBuild.platforms.filter { entry.prebuilt[$0] == nil }
        if !missingPlatforms.isEmpty {
            print("Prebuilt \(library.rawValue) is missing platforms \(missingPlatforms.map(\.rawValue).joined(separator: ",")); building from source")
            return nil
        }
        let missingFrameworks = library.frameworks.filter { !entry.frameworks.contains($0) }
        if !missingFrameworks.isEmpty {
            print("Prebuilt \(library.rawValue) is missing frameworks \(missingFrameworks.joined(separator: ",")); building from source")
            return nil
        }
        return (manifest, entry)
    }

    /// Unpack the per-platform prebuilt zips of this library and lay them out under
    /// `dist/<library>/` as if they had just been compiled. No xcframework is produced
    /// locally: the published xcframeworks are what `Package.swift` links against.
    private func restoreFromPrebuilt(entry: BinaryManifest.Entry, manifest: BinaryManifest) throws {
        print("Restore \(library.rawValue) from prebuilt binaries (key \(entry.key))")
        let unpackedURL = URL.currentDirectory + "\(library.rawValue)-prebuilt-\(entry.key)"
        try FileManager.default.createDirectory(atPath: unpackedURL.path, withIntermediateDirectories: true, attributes: nil)
        for platform in BaseBuild.platforms {
            guard let asset = entry.prebuilt[platform] else {
                continue
            }
            let zipURL = unpackedURL + asset
            // delete invalid downloaded files
            let attributes = try? FileManager.default.attributesOfItem(atPath: zipURL.path)
            if let fileSize = attributes?[FileAttributeKey.size] as? UInt64, fileSize <= 0 {
                try? FileManager.default.removeItem(at: zipURL)
            }
            if !FileManager.default.fileExists(atPath: zipURL.path) {
                do {
                    try Utility.launch(path: "wget", arguments: ["-O", asset, manifest.url(ofAsset: asset)], currentDirectoryURL: unpackedURL)
                } catch {
                    try? FileManager.default.removeItem(at: zipURL)
                    throw error
                }
            }
            // the per-platform zips share one layout, so unpacking them all into the same
            // directory rebuilds exactly the tree `packageRelease()` would have produced
            if !FileManager.default.fileExists(atPath: (unpackedURL + ["lib", platform.rawValue]).path) {
                try Utility.launch(path: "/usr/bin/unzip", arguments: ["-o", "-q", asset], currentDirectoryURL: unpackedURL)
            }
        }

        try? FileManager.default.removeItem(at: URL.currentDirectory + library.rawValue)
        try? FileManager.default.createDirectory(atPath: (URL.currentDirectory + library.rawValue).path, withIntermediateDirectories: true, attributes: nil)
        restorePackagedArtifacts(from: unpackedURL)
    }

    /// The `lib` directory holding one platform/arch slice inside an unpacked
    /// `-all.zip` tree. Most prebuilt releases use `ZipBaseBuild`'s
    /// `<root>/lib/<platform>/thin/<arch>/lib` layout; MoltenVK nests the slices
    /// inside its xcframework instead.
    func unpackedThinLib(root: URL, platform: PlatformType, arch: ArchType) -> URL {
        root + ["lib"] + [platform.rawValue, "thin", arch.rawValue, "lib"]
    }

    /// Copy an unpacked `<library>-all*.zip` tree into `dist/<library>/<platform>/thin/<arch>/`.
    /// Shared by prebuilt restores of self-built libraries, `ZipBaseBuild` and `BuildVulkan`.
    func restorePackagedArtifacts(from unpackedURL: URL) {
        for platform in BaseBuild.platforms {
            for arch in architectures(platform) {
                // restore lib
                let srcThinLibPath = unpackedThinLib(root: unpackedURL, platform: platform, arch: arch)
                // ignore if platform not support
                if !FileManager.default.fileExists(atPath: srcThinLibPath.path) {
                    continue
                }
                let destThinPath = thinDir(platform: platform, arch: arch)
                let destThinLibPath = destThinPath + ["lib"]
                try? FileManager.default.createDirectory(atPath: destThinPath.path, withIntermediateDirectories: true, attributes: nil)
                try? FileManager.default.copyItem(at: srcThinLibPath, to: destThinLibPath)

                // restore include
                let srcIncludePath = unpackedURL + ["include"]
                let destIncludePath = destThinPath + ["include"]
                try? FileManager.default.copyItem(at: srcIncludePath, to: destIncludePath)

                // restore pkgconfig
                let srcPkgConfigPath = unpackedURL + ["pkgconfig-example", platform.rawValue, arch.rawValue]
                let destPkgConfigPath = destThinPath + ["lib", "pkgconfig"]
                try? FileManager.default.copyItem(at: srcPkgConfigPath, to: destPkgConfigPath)
                Utility.listAllFiles(in: destPkgConfigPath).forEach { file in
                    if let data = FileManager.default.contents(atPath: file.path), var str = String(data: data, encoding: .utf8) {
                        str = str.replacingOccurrences(of: "/path/to/workdir", with: URL.currentDirectory.path)
                        try! str.write(toFile: file.path, atomically: true, encoding: .utf8)
                    }
                }
            }
        }
    }

    func architectures(_ platform: PlatformType) -> [ArchType] {
        platform.architectures
    }

    func platforms() -> [PlatformType] {
        BaseBuild.platforms
    }

    func build(platform: PlatformType, arch: ArchType) throws {
        let buildURL = scratch(platform: platform, arch: arch)
        try? FileManager.default.createDirectory(at: buildURL, withIntermediateDirectories: true, attributes: nil)
        let environ = environment(platform: platform, arch: arch)
        if FileManager.default.fileExists(atPath: (directoryURL + "meson.build").path) {
            if Utility.shell("which meson") == nil {
                Utility.shell("brew install meson")
            }
            if Utility.shell("which ninja") == nil {
                Utility.shell("brew install ninja")
            }
            

            let crossFile = createMesonCrossFile(platform: platform, arch: arch)
            let meson = Utility.shell("which meson", isOutput: true)!
            try Utility.launch(path: meson, arguments: ["setup", buildURL.path, "--cross-file=\(crossFile.path)"] + arguments(platform: platform, arch: arch), currentDirectoryURL: directoryURL, environment: environ)
            try Utility.launch(path: meson, arguments: ["compile", "--clean"], currentDirectoryURL: buildURL, environment: environ)
            try Utility.launch(path: meson, arguments: ["compile", "--verbose"], currentDirectoryURL: buildURL, environment: environ)
            try Utility.launch(path: meson, arguments: ["install"], currentDirectoryURL: buildURL, environment: environ)
        } else {
            try configure(buildURL: buildURL, environ: environ, platform: platform, arch: arch)
            try Utility.launch(path: "/usr/bin/make", arguments: ["-j8"], currentDirectoryURL: buildURL, environment: environ)
            try Utility.launch(path: "/usr/bin/make", arguments: ["-j8", "install"], currentDirectoryURL: buildURL, environment: environ)
        }
    }

    /// FFmpeg is the only source build without a `meson.build`: it ships an
    /// in-tree `configure` and is installed with make.
    func configure(buildURL: URL, environ: [String: String], platform: PlatformType, arch: ArchType) throws {
        var arguments = [
            "--prefix=\(thinDir(platform: platform, arch: arch).path)",
        ]
        arguments.append(contentsOf: self.arguments(platform: platform, arch: arch))
        try Utility.launch(executableURL: directoryURL + "configure", arguments: arguments, currentDirectoryURL: buildURL, environment: environ)
    }

    func environment(platform: PlatformType, arch: ArchType) -> [String: String] {
        let cFlags = cFlags(platform: platform, arch: arch).joined(separator: " ")
        let ldFlags = ldFlags(platform: platform, arch: arch).joined(separator: " ")
        let pkgConfigPath = platform.pkgConfigPath(arch: arch)
        let pkgConfigPathDefault = Utility.shell("pkg-config --variable pc_path pkg-config", isOutput: true)!
        return [
            "LC_CTYPE": "C",
            "CC": "/usr/bin/clang",
            "CXX": "/usr/bin/clang++",
            // "SDKROOT": platform.sdk.lowercased(),
            "CURRENT_ARCH": arch.rawValue,
            "CFLAGS": cFlags,
            // makefile can't use CPPFLAGS
            "CPPFLAGS": cFlags,
            // 这个要加，不然cmake在编译maccatalyst 会有问题
            "CXXFLAGS": cFlags,
            "ASMFLAGS": cFlags,
            "LDFLAGS": ldFlags,
            "PKG_CONFIG_LIBDIR": pkgConfigPath + pkgConfigPathDefault,
            "PATH": BaseBuild.defaultPath,
        ]
    }

    func cFlags(platform: PlatformType, arch: ArchType) -> [String] {
        var cFlags = platform.cFlags(arch: arch)
        let librarys = flagsDependencelibrarys()
        for library in librarys {
            let path = URL.currentDirectory + [library.rawValue, platform.rawValue, "thin", arch.rawValue]
            if FileManager.default.fileExists(atPath: path.path) {
                cFlags.append("-I\(path.path)/include")
            }
        }
        return cFlags
    }

    func ldFlags(platform: PlatformType, arch: ArchType) -> [String] {
        var ldFlags = platform.ldFlags(arch: arch)
        let librarys = flagsDependencelibrarys()
        for library in librarys {
            let path = URL.currentDirectory + [library.rawValue, platform.rawValue, "thin", arch.rawValue]
            if FileManager.default.fileExists(atPath: path.path) {
                var libname = library.rawValue
                if libname.hasPrefix("lib") {
                    libname = String(libname.dropFirst(3))
                }
                ldFlags.append("-L\(path.path)/lib")
                ldFlags.append("-l\(libname)")
            }
        }
        return ldFlags
    }

    func flagsDependencelibrarys() -> [Library] {
        []
    }


    func arguments(platform: PlatformType, arch: ArchType) -> [String] {
        return []
    }

    func frameworks() throws -> [String] {
        [library.rawValue]
    }

    /// Framework display names (`libfoo` becomes `Libfoo`); the same spelling
    /// `scripts/keys.py` records in artifacts.json and `Package.swift` links.
    func frameworkNames() throws -> [String] {
        try frameworks().map { $0.hasPrefix("lib") ? "Lib" + $0.dropFirst(3) : $0 }
    }

    func createXCFramework() throws {
        // clean all old xcframework
        try? Utility.removeFiles(extensions: [".xcframework"], currentDirectoryURL: self.xcframeworkDirectoryURL)

        for framework in try frameworkNames() {
            var frameworkGenerated = [PlatformType: String]()
            for platform in BaseBuild.platforms {
                if let frameworkPath = try createFramework(framework: framework, platform: platform) {
                    frameworkGenerated[platform] = frameworkPath
                }
            }
            try buildXCFramework(name: framework, paths: Array(frameworkGenerated.values))
        }
    }

    private func buildXCFramework(name: String, paths: [String]) throws {
        if paths.isEmpty {
            return
        }

        var arguments = ["-create-xcframework"]
        for frameworkPath in paths {
            arguments.append("-framework")
            arguments.append(frameworkPath)
        }
        arguments.append("-output")
        let XCFrameworkFile = self.xcframeworkDirectoryURL + [name + ".xcframework"]
        arguments.append(XCFrameworkFile.path)
        if FileManager.default.fileExists(atPath: XCFrameworkFile.path) {
            try? FileManager.default.removeItem(at: XCFrameworkFile)
        }
        try Utility.launch(path: "/usr/bin/xcodebuild", arguments: arguments)
    }

    func createFramework(framework: String, platform: PlatformType) throws -> String? {
        let platformDir = URL.currentDirectory + [library.rawValue, platform.rawValue]
        if !FileManager.default.fileExists(atPath: platformDir.path) {
            return nil
        }
        let frameworkDir = URL.currentDirectory + [library.rawValue, platform.rawValue, "\(framework).framework"]
        if !platforms().contains(platform) {
            if FileManager.default.fileExists(atPath: frameworkDir.path) {
                return frameworkDir.path
            } else {
                return nil
            }
        }
        try? FileManager.default.removeItem(at: frameworkDir)
        try FileManager.default.createDirectory(at: frameworkDir, withIntermediateDirectories: true, attributes: nil)
        var arguments = ["-create"]
        for arch in platform.architectures {
            let prefix = thinDir(platform: platform, arch: arch)
            if !FileManager.default.fileExists(atPath: prefix.path) {
                return nil
            }
            let libname = framework.hasPrefix("lib") || framework.hasPrefix("Lib") ? framework : "lib" + framework
            var libPath = prefix + ["lib", "\(libname).a"]
            if !FileManager.default.fileExists(atPath: libPath.path) {
                libPath = prefix + ["lib", "\(libname).dylib"]
            }
            arguments.append(libPath.path)
            var headerURL: URL = prefix + "include" + framework
            if !FileManager.default.fileExists(atPath: headerURL.path) {
                headerURL = prefix + "include"
            }
            try? FileManager.default.copyItem(at: headerURL, to: frameworkDir + "Headers")
        }
        arguments.append("-output")
        arguments.append((frameworkDir + framework).path)
        try Utility.launch(path: "/usr/bin/lipo", arguments: arguments)
        try FileManager.default.createDirectory(at: frameworkDir + "Modules", withIntermediateDirectories: true, attributes: nil)
        var modulemap = """
        framework module \(framework) [system] {
            umbrella "."

        """
        frameworkExcludeHeaders(framework).forEach { header in
            modulemap += """
                exclude header "\(header).h"

            """
        }
        modulemap += """
            export *
        }
        """
        FileManager.default.createFile(atPath: frameworkDir.path + "/Modules/module.modulemap", contents: modulemap.data(using: .utf8), attributes: nil)
        // Setting the minimum version to 100.0 is required for uploading a static framework to the App Store after Xcode 15.4
        // Fix: ITMS-90208: "Invalid Bundle. The bundle xxx.framework does not support the minimum OS Version specified in the Info.plist."
        // It was originally using `platform.minVersion`
        createPlist(path: frameworkDir.path + "/Info.plist", name: framework, minVersion: "100.0", platform: platform.sdk)
        try fixShallowBundles(framework: framework, platform: platform, frameworkDir: frameworkDir)
        return frameworkDir.path
    }

    // Fix shallow bundles for Xcode 26, only for macOS frameworks
    func fixShallowBundles(framework: String, platform: PlatformType, frameworkDir: URL) throws {
        guard platform == .macos else { return }

        let infoPlistPath = frameworkDir + "Info.plist"
        let versionsPath = frameworkDir + "Versions"
        
        // Check if this is a shallow bundle that needs fixing
        var isDirectory: ObjCBool = false
        let frameworkExists = FileManager.default.fileExists(atPath: frameworkDir.path, isDirectory: &isDirectory)
        let hasInfoPlist = FileManager.default.fileExists(atPath: infoPlistPath.path)
        let hasVersions = FileManager.default.fileExists(atPath: versionsPath.path, isDirectory: &isDirectory) && isDirectory.boolValue
        
        if frameworkExists && hasInfoPlist && !hasVersions {
            print("Fixing \(framework).framework bundle structure...")
            
            // Create proper bundle structure
            let versionAResourcesPath = frameworkDir + ["Versions", "A", "Resources"]
            try FileManager.default.createDirectory(at: versionAResourcesPath, withIntermediateDirectories: true, attributes: nil)
            
            // Move Info.plist to proper location
            let newInfoPlistPath = versionAResourcesPath + "Info.plist"
            try FileManager.default.moveItem(at: infoPlistPath, to: newInfoPlistPath)
            
            // Move framework binary to proper location
            let binaryPath = frameworkDir + framework
            let newBinaryPath = frameworkDir + ["Versions", "A", framework]
            if FileManager.default.fileExists(atPath: binaryPath.path) {
                try FileManager.default.moveItem(at: binaryPath, to: newBinaryPath)
            }
            
            // Move LICENSE if exists
            let licensePath = frameworkDir + "LICENSE"
            if FileManager.default.fileExists(atPath: licensePath.path) {
                let newLicensePath = frameworkDir + ["Versions", "A", "LICENSE"]
                try FileManager.default.moveItem(at: licensePath, to: newLicensePath)
            }
            
            // Create symbolic links
            let currentLinkPath = frameworkDir + ["Versions", "Current"]
            try? FileManager.default.removeItem(at: currentLinkPath)
            try FileManager.default.createSymbolicLink(atPath: currentLinkPath.path, withDestinationPath: "A")
            
            let binaryLinkPath = frameworkDir + framework
            try? FileManager.default.removeItem(at: binaryLinkPath)
            try FileManager.default.createSymbolicLink(atPath: binaryLinkPath.path, withDestinationPath: "Versions/Current/\(framework)")
            
            let resourcesLinkPath = frameworkDir + "Resources"
            try? FileManager.default.removeItem(at: resourcesLinkPath)
            try FileManager.default.createSymbolicLink(atPath: resourcesLinkPath.path, withDestinationPath: "Versions/Current/Resources")
            
            print("\(framework).framework structure fixed")
        }
    }

    func thinDir(library: Library, platform: PlatformType, arch: ArchType) -> URL {
        URL.currentDirectory + [library.rawValue, platform.rawValue, "thin", arch.rawValue]
    }

    func thinDir(platform: PlatformType, arch: ArchType) -> URL {
        thinDir(library: library, platform: platform, arch: arch)
    }

    func scratch(platform: PlatformType, arch: ArchType) -> URL {
        URL.currentDirectory + [library.rawValue, platform.rawValue, "scratch", arch.rawValue]
    }

    func frameworkExcludeHeaders(_: String) -> [String] {
        []
    }

    private func createPlist(path: String, name: String, minVersion: String, platform: String) {
        let identifier = "com.mpvkit." + normalizeBundleIdentifier(name)
        let content = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
        <key>CFBundleDevelopmentRegion</key>
        <string>en</string>
        <key>CFBundleExecutable</key>
        <string>\(name)</string>
        <key>CFBundleIdentifier</key>
        <string>\(identifier)</string>
        <key>CFBundleInfoDictionaryVersion</key>
        <string>6.0</string>
        <key>CFBundleName</key>
        <string>\(name)</string>
        <key>CFBundlePackageType</key>
        <string>FMWK</string>
        <key>CFBundleShortVersionString</key>
        <string>87.88.520</string>
        <key>CFBundleVersion</key>
        <string>87.88.520</string>
        <key>CFBundleSignature</key>
        <string>????</string>
        <key>MinimumOSVersion</key>
        <string>\(minVersion)</string>
        <key>CFBundleSupportedPlatforms</key>
        <array>
        <string>\(platform)</string>
        </array>
        <key>NSPrincipalClass</key>
        <string></string>
        </dict>
        </plist>
        """
        FileManager.default.createFile(atPath: path, contents: content.data(using: .utf8), attributes: nil)
    }

    // CFBundleIdentifier must contain only alphanumerics(a-z), dots(.), hyphens(-) 
    private func normalizeBundleIdentifier(_ identifier: String) -> String {
        return identifier.replacingOccurrences(of: "_", with: "-")
    }


    private func createMesonCrossFile(platform: PlatformType, arch: ArchType) -> URL {
        let url = scratch(platform: platform, arch: arch)
        let crossFile = url + "crossFile.meson"
        let prefix = thinDir(platform: platform, arch: arch)
        let cFlags = cFlags(platform: platform, arch: arch).map {
            "'" + $0 + "'"
        }.joined(separator: ", ")
        let ldFlags = ldFlags(platform: platform, arch: arch).map {
            "'" + $0 + "'"
        }.joined(separator: ", ")
        let content = """
        [binaries]
        c = '/usr/bin/clang'
        cpp = '/usr/bin/clang++'
        objc = '/usr/bin/clang'
        objcpp = '/usr/bin/clang++'
        ar = '\(platform.xcrunFind(tool: "ar"))'
        strip = '\(platform.xcrunFind(tool: "strip"))'
        pkg-config = 'pkg-config'

        [properties]
        has_function_printf = true
        has_function_hfkerhisadf = false

        [host_machine]
        system = 'darwin'
        subsystem = '\(platform.mesonSubSystem)'
        kernel = 'xnu'
        cpu_family = '\(arch.cpuFamily)'
        cpu = '\(arch.targetCpu)'
        endian = 'little'

        [built-in options]
        default_library = 'static'
        buildtype = 'release'
        b_ndebug = 'true'
        prefix = '\(prefix.path)'
        c_args = [\(cFlags)]
        cpp_args = [\(cFlags)]
        objc_args = [\(cFlags)]
        objcpp_args = [\(cFlags)]
        c_link_args = [\(ldFlags)]
        cpp_link_args = [\(ldFlags)]
        objc_link_args = [\(ldFlags)]
        objcpp_link_args = [\(ldFlags)]
        """
        FileManager.default.createFile(atPath: crossFile.path, contents: content.data(using: .utf8), attributes: nil)
        return crossFile
    }

    func packageRelease() throws {
        let releaseDirPath = URL.currentDirectory + ["release"]
        if !FileManager.default.fileExists(atPath: releaseDirPath.path) {
            try? FileManager.default.createDirectory(at: releaseDirPath, withIntermediateDirectories: true, attributes: nil)
        }
        let releaseLibPath = releaseDirPath + [library.rawValue]
        try? FileManager.default.removeItem(at: releaseLibPath)

        // copy static libraries
        for platform in BaseBuild.platforms {
            for arch in architectures(platform) {
                 let thinLibPath = thinDir(platform: platform, arch: arch) + ["lib"]
                 if !FileManager.default.fileExists(atPath: thinLibPath.path) {
                     continue
                 }
                 let staticLibraries = try FileManager.default.contentsOfDirectory(atPath: thinLibPath.path).filter { $0.hasSuffix(".a") }

                 let releaseThinLibPath = releaseDirPath + [library.rawValue, "lib", platform.rawValue, "thin", arch.rawValue, "lib"]
                 try? FileManager.default.createDirectory(at: releaseThinLibPath, withIntermediateDirectories: true, attributes: nil)
                 for lib in staticLibraries {
                    let sourceURL = thinLibPath + [lib]
                    let destinationURL = releaseThinLibPath + [lib]
                    try FileManager.default.copyItem(at: sourceURL, to: destinationURL)
                }
            }
        }

        // copy includes
        guard let firstPlatform = getFirstSuccessPlatform() else { return }
        let firstArch = architectures(firstPlatform).first!
        let includePath = thinDir(platform: firstPlatform, arch: firstArch) + ["include"]
        let destIncludePath = releaseDirPath + [library.rawValue, "include"]
        try FileManager.default.copyItem(at: includePath, to: destIncludePath)


        // copy pkg-config file example
        try packagePkgConfigRelease()

        let names = try frameworkNames()

        // zip build artifacts when there are frameworks to generate
        if !names.isEmpty {
            let sourceLib = releaseDirPath + [library.rawValue]
            let destZipLibPath = releaseDirPath + [library.rawValue + "-all.zip"]
            try? FileManager.default.removeItem(at: destZipLibPath)
            try Self.deterministicZip(entry: "./", zipFile: destZipLibPath, currentDirectoryURL: sourceLib)
        }

        // zip xcframeworks
        for framework in names {
            // clean old zip files
            try? FileManager.default.removeItem(at: releaseDirPath + [framework + ".xcframework.zip"])
            try? FileManager.default.removeItem(at: releaseDirPath + [framework + ".xcframework.checksum.txt"])

            let XCFrameworkFile =  framework + ".xcframework"
            let zipFile = releaseDirPath + [framework + ".xcframework.zip"]
            let checksumFile = releaseDirPath + [framework + ".xcframework.checksum.txt"]
            try Self.deterministicZip(entry: XCFrameworkFile, zipFile: zipFile, currentDirectoryURL: self.xcframeworkDirectoryURL)
            Utility.shell("swift package compute-checksum \(zipFile.path) > \(checksumFile.path)")
        }
    }

    /// Zip with reproducible bytes. The release publishes content-addressed,
    /// immutable assets and skips re-uploading an existing name, so a rebuild
    /// of the same key MUST produce the identical archive or the recorded
    /// checksum drifts from the published bytes (this happened: zip stores
    /// each file's DOS mtime, so every rebuild differed). Normalize mtimes,
    /// strip extended attributes (-X), keep symlinks (-y) and feed entries in
    /// sorted order.
    static func deterministicZip(entry: String, zipFile: URL, currentDirectoryURL: URL) throws {
        _ = Utility.shell("find \(entry) -exec touch -h -t 202001010000 {} +", currentDirectoryURL: currentDirectoryURL)
        _ = try Utility.launch(
            path: "/bin/bash",
            arguments: ["-c", "find \(entry) \\( -type f -o -type l \\) -print | LC_ALL=C sort | zip -q -X -y '\(zipFile.path)' -@"],
            currentDirectoryURL: currentDirectoryURL
        )
    }

    func packagePkgConfigRelease() throws {
        let releaseDirPath = URL.currentDirectory + ["release"]
        // copy pkg-config file example
        for platform in BaseBuild.platforms {
            for arch in architectures(platform) {
                let thinLibPath = thinDir(platform: platform, arch: arch) + ["lib"]
                let pkgconfigPath = thinLibPath + ["pkgconfig"]
                if !FileManager.default.fileExists(atPath: pkgconfigPath.path) {
                    continue
                }
                let destPkgConfigDir = releaseDirPath + [library.rawValue, "pkgconfig-example", platform.rawValue]
                let destPkgConfigPath = destPkgConfigDir + arch.rawValue
                try? FileManager.default.createDirectory(at: destPkgConfigDir, withIntermediateDirectories: true, attributes: nil)
                try FileManager.default.copyItem(at: pkgconfigPath, to: destPkgConfigPath)

                let pkgconfigFiles = Utility.listAllFiles(in: destPkgConfigPath)
                for file in pkgconfigFiles {
                    if let data = FileManager.default.contents(atPath: file.path), var str = String(data: data, encoding: .utf8) {
                        str = str.replacingOccurrences(of: URL.currentDirectory.path, with: "/path/to/workdir")
                        try! str.write(toFile: file.path, atomically: true, encoding: .utf8)
                    }
                }
            }
        }
    }

    func getFirstSuccessPlatform() -> PlatformType? {
        for platform in BaseBuild.platforms {
            let firstArch = architectures(platform).first!
            let thinPath = thinDir(platform: platform, arch: firstArch)
            if FileManager.default.fileExists(atPath: thinPath.path) {
                return platform
            }
        }

        return nil
    }
}

class ZipBaseBuild : BaseBuild {

    override func beforeBuild() throws {
        // unzip builded static library
        let outputFileName = "\(library.rawValue).zip"
        let outputFile = directoryURL + outputFileName
        // delete invalid downloaded files
        let attributes = try? FileManager.default.attributesOfItem(atPath: outputFile.path)
        if let fileSize = attributes?[FileAttributeKey.size] as? UInt64, fileSize <= 0 {
            try? FileManager.default.removeItem(atPath: directoryURL.path)
        }
        try! FileManager.default.createDirectory(atPath: directoryURL.path, withIntermediateDirectories: true, attributes: nil)

        if !FileManager.default.fileExists(atPath: outputFile.path) {
            try! Utility.launch(path: "wget", arguments: ["-O", outputFileName, library.url], currentDirectoryURL: directoryURL)
            try! Utility.launch(path: "/usr/bin/unzip", arguments: ["-o",outputFileName], currentDirectoryURL: directoryURL)
        }
    }

    override func buildALL() throws {
        try beforeBuild()
        try? FileManager.default.removeItem(at: URL.currentDirectory + library.rawValue)
        try? FileManager.default.removeItem(at: directoryURL.appendingPathExtension("log"))
        try? FileManager.default.createDirectory(atPath: (URL.currentDirectory + library.rawValue).path, withIntermediateDirectories: true, attributes: nil)
        restorePackagedArtifacts(from: directoryURL)
    }
}

enum PlatformType: String, CaseIterable {
    case xros, xrsimulator, maccatalyst, macos, isimulator, tvsimulator, tvos, ios
    var minVersion: String {
        switch self {
        case .ios, .isimulator:
            return "14.0"
        case .tvos, .tvsimulator:
            return "14.0"
        case .macos:
            return "11.0"
        case .maccatalyst:
            // return "14.0"
            return ""
        case .xros, .xrsimulator:
            return "1.0"
        }
    }

    var frameworkName: String {
        switch self {
        case .ios:
            return "ios-arm64"
        case .maccatalyst:
            return "ios-arm64_x86_64-maccatalyst"
        case .isimulator:
            return "ios-arm64_x86_64-simulator"
        case .macos:
            return "macos-arm64_x86_64"
        case .tvos:
            // 保持和xcode一致：https://github.com/KhronosGroup/MoltenVK/issues/431#issuecomment-771137085
            return "tvos-arm64_arm64e"
        case .tvsimulator:
            return "tvos-arm64_x86_64-simulator"
        case .xros:
            return "xros-arm64"
        case .xrsimulator:
            return "xros-arm64_x86_64-simulator"
        }
    }

    // xcodebuild default ARCHS = "$(ARCHS_STANDARD_64_BIT)" only build arm64e for tvos
    var architectures: [ArchType] {
        switch self {
        case .ios, .xros:
            return [.arm64]
        case .tvos:
            return [.arm64, .arm64e]
        case .xrsimulator:
            return [.arm64]
        case .isimulator, .tvsimulator:
            return [.arm64, .x86_64]  
        case .macos:
            // macos 不能用arm64，不然打包release包会报错，不能通过
            #if arch(x86_64)
            return [.x86_64, .arm64]
            #else
            return [.arm64, .x86_64]
            #endif
        case .maccatalyst:
            return [.arm64, .x86_64]
        }
    }

    func deploymentTarget(_ arch: ArchType) -> String {
        switch self {
        case .ios, .tvos, .macos, .xros:
            return "\(arch.targetCpu)-apple-\(rawValue)\(minVersion)"
        case .maccatalyst:
            return "\(arch.targetCpu)-apple-ios-macabi"
        case .isimulator:
            return PlatformType.ios.deploymentTarget(arch) + "-simulator"
        case .tvsimulator:
            return PlatformType.tvos.deploymentTarget(arch) + "-simulator"
        case .xrsimulator:
            return PlatformType.xros.deploymentTarget(arch) + "-simulator"
        }
    }


    private var osVersionMin: String {
        switch self {
        case .ios, .tvos:
            return "-m\(rawValue)-version-min=\(minVersion)"
        case .macos:
            return "-mmacosx-version-min=\(minVersion)"
        case .isimulator:
            return "-mios-simulator-version-min=\(minVersion)"
        case .tvsimulator:
            return "-mtvos-simulator-version-min=\(minVersion)"
        case .maccatalyst, .xros, .xrsimulator:
            return ""
            // return "-miphoneos-version-min=\(minVersion)"
        }
    }

    var sdk : String {
        switch self {
        case .ios:
            return "iPhoneOS"
        case .isimulator:
            return "iPhoneSimulator"
        case .tvos:
            return "AppleTVOS"
        case .tvsimulator:
            return "AppleTVSimulator"
        case .macos:
            return "MacOSX"
        case .maccatalyst:
            return "MacOSX"
        case .xros:
            return "XROS"
        case .xrsimulator:
            return "XRSimulator"
        }
    }

    var isysroot: String {
        xcrunFind(tool: "--show-sdk-path")
    }

    var mesonSubSystem: String {
        switch self {
        case .isimulator:
            return "ios-simulator"
        case .tvsimulator:
            return "tvos-simulator"
        case .xrsimulator:
            return "xros-simulator"
        default:
            return rawValue
        }
    }

    func ldFlags(arch: ArchType) -> [String] {
        // ldFlags的关键参数要跟cFlags保持一致，不然会在ld的时候不通过。
        var flags = ["-lc++", "-arch", arch.rawValue, "-isysroot", isysroot, "-target", deploymentTarget(arch), osVersionMin]
        // maccatalyst的vulkan库需要加载UIKit框架
        if self == .maccatalyst {
            flags += ["-iframework", "\(isysroot)/System/iOSSupport/System/Library/Frameworks"]
        }
        return flags
    }


    func cFlags(arch: ArchType) -> [String] {
        var cflags = ["-arch", arch.rawValue, "-isysroot", isysroot, "-target", deploymentTarget(arch), osVersionMin]
//        if self == .macos || self == .maccatalyst {
        // 不能同时有强符合和弱符号出现
        // cflags.append("-fno-common")
//        }
        if self == .maccatalyst {
            cflags += ["-iframework", "\(isysroot)/System/iOSSupport/System/Library/Frameworks"]
        }
        if self == .tvos || self == .tvsimulator {
            cflags.append("-DHAVE_FORK=0")
        }
        return cflags
    }

    func xcrunFind(tool: String) -> String {
        try! Utility.launch(path: "/usr/bin/xcrun", arguments: ["--sdk", sdk.lowercased(), "--find", tool], isOutput: true)
    }

    func pkgConfigPath(arch: ArchType) -> String {
        var pkgConfigPath = ""
        for lib in Library.allCases {
            let path = URL.currentDirectory + [lib.rawValue, rawValue, "thin", arch.rawValue]
            if FileManager.default.fileExists(atPath: path.path) {
                pkgConfigPath += "\(path.path)/lib/pkgconfig:"
            }
        }
        return pkgConfigPath
    }
}

enum ArchType: String, CaseIterable {
    // swiftlint:disable identifier_name
    case arm64, x86_64, arm64e
    // swiftlint:enable identifier_name
    var executable: Bool {
        guard let architecture = Bundle.main.executableArchitectures?.first?.intValue else {
            return false
        }
        // NSBundleExecutableArchitectureARM64
        if architecture == 0x0100_000C, self == .arm64 {
            return true
        } else if architecture == NSBundleExecutableArchitectureX86_64, self == .x86_64 {
            return true
        }
        return false
    }


    var cpuFamily: String {
        switch self {
        case .arm64, .arm64e:
            return "aarch64"
        case .x86_64:
            return "x86_64"
        }
    }

    var targetCpu: String {
        switch self {
        case .arm64, .arm64e:
            return "arm64"
        case .x86_64:
            return "x86_64"
        }
    }
}



enum Utility {
    @discardableResult
    static func shell(_ command: String, isOutput : Bool = false, currentDirectoryURL: URL? = nil, environment: [String: String] = [:]) -> String? {
        do {
            return try launch(executableURL: URL(fileURLWithPath: "/bin/bash"), arguments: ["-c", command], isOutput: isOutput, currentDirectoryURL: currentDirectoryURL, environment: environment)
        } catch {
            print(error.localizedDescription)
            return nil
        }
    }

    @discardableResult
    static func launch(path: String, arguments: [String], isOutput: Bool = false, currentDirectoryURL: URL? = nil, environment: [String: String] = [:]) throws -> String {
        if !path.hasPrefix("/") {
            let execPath = Utility.shell("which \(path)", isOutput: true)!
            if execPath.isEmpty {
                throw NSError(domain: "[\(path)] not found", code: 1)
            }
            return try launch(executableURL: URL(fileURLWithPath: execPath), arguments: arguments, isOutput: isOutput, currentDirectoryURL: currentDirectoryURL, environment: environment)
        } else {
            return try launch(executableURL: URL(fileURLWithPath: path), arguments: arguments, isOutput: isOutput, currentDirectoryURL: currentDirectoryURL, environment: environment)
        }
    }

    @discardableResult
    static func launch(executableURL: URL, arguments: [String], isOutput: Bool = false, currentDirectoryURL: URL? = nil, environment: [String: String] = [:]) throws -> String {
        let task = Process()
        var environment = environment
        // for homebrew 1.12
        if ProcessInfo.processInfo.environment.keys.contains("HOME") {
            environment["HOME"] = ProcessInfo.processInfo.environment["HOME"]
        }
        if !environment.keys.contains("PATH") {
            environment["PATH"] = BaseBuild.defaultPath
        }
        task.environment = environment

        var outputFileHandle: FileHandle?
        var logURL: URL?
        var outputBuffer = Data()
        let outputPipe = Pipe()
        let errorPipe = Pipe()
        task.standardOutput = outputPipe
        task.standardError = errorPipe
        
        if let curURL = currentDirectoryURL {
            // output to file
            logURL = curURL.appendingPathExtension("log")
            if !FileManager.default.fileExists(atPath: logURL!.path) {
                FileManager.default.createFile(atPath: logURL!.path, contents: nil)
            }

            outputFileHandle = try FileHandle(forWritingTo: logURL!)
            outputFileHandle?.seekToEndOfFile()
        }
        outputPipe.fileHandleForReading.readabilityHandler = { fileHandle in
            let data = fileHandle.availableData

            if !data.isEmpty {
                outputBuffer.append(data)
                if let outputString = String(data: data, encoding: .utf8) {
                    if isOutput {
                        print(outputString.trimmingCharacters(in: .newlines))
                    }

                    // Write to file simultaneously.
                    outputFileHandle?.write(data)
                }
            } else {
                // Close the read capability processing program and clean up resources.
                fileHandle.readabilityHandler = nil
                fileHandle.closeFile()
            }
        }
        errorPipe.fileHandleForReading.readabilityHandler = { fileHandle in
            let data = fileHandle.availableData

            if !data.isEmpty {
                if let outputString = String(data: data, encoding: .utf8) {
                    print(outputString.trimmingCharacters(in: .newlines))

                    // Write to file simultaneously.
                    outputFileHandle?.write(data)
                }
            } else {
                // Close the read capability processing program and clean up resources.
                fileHandle.readabilityHandler = nil
                fileHandle.closeFile()
            }
        }
    
        task.arguments = arguments
        var log = executableURL.path + " " + arguments.joined(separator: " ") + " environment: " + environment.description
        if let currentDirectoryURL {
            log += " url: \(currentDirectoryURL)"
        }
        print(log)
        outputFileHandle?.write("\(log)\n".data(using: .utf8)!)
        task.currentDirectoryURL = currentDirectoryURL
        task.executableURL = executableURL
        try task.run()
        task.waitUntilExit()
        if task.terminationStatus == 0 {
            if isOutput {
                let result = String(data: outputBuffer, encoding: .utf8)?.trimmingCharacters(in: .newlines) ?? ""
                return result
            } else {
                return ""
            }
        } else {
            if let logURL = logURL {
                // print log when run in GitHub Action
                if ProcessInfo.processInfo.environment.keys.contains("GITHUB_ACTION") {
                    // if build FFmpeg failed, print the ffbuild/config.log content
                    if logURL.path.contains("FFmpeg") {
                        let ffbuildLogURL = logURL
                            .deletingPathExtension()
                            .appendingPathComponent("ffbuild/config.log")
                        if FileManager.default.fileExists(atPath: ffbuildLogURL.path) {
                            if let content = String(data: try Data(contentsOf: ffbuildLogURL), encoding: .utf8) {
                                print("############# \(ffbuildLogURL) CONTENT BEGIN #############")
                                print(content)
                                print("#############  \(ffbuildLogURL) CONTENT END #############")
                            }
                        }
                    }

                    if let content = String(data: try Data(contentsOf: logURL), encoding: .utf8) {
                        print("############# \(logURL) CONTENT BEGIN #############")
                        print(content)
                        print("#############  \(logURL) CONTENT END #############")
                        if #available(macOS 13.0, *) {
                            let regErrLogPath = try Regex("A full log can be found at\\s+?(/.*\\.txt)")
                            if let firstMatch = content.firstMatch(of: regErrLogPath) {
                                let errPath = "\(firstMatch[1].value ?? "")"
                                if !errPath.isEmpty {
                                    print("############# \(errPath) CONTENT BEGIN #############")
                                    let content = Utility.shell("cat \(errPath)", isOutput: true)
                                    print(content ?? "")
                                    print("#############  \(errPath) CONTENT END #############")
                                }
                            }
                        }
                    }
                    
                }
                print("please view log file for detail: \(logURL)\n")
            }
            throw NSError(domain: "\(executableURL.lastPathComponent) execute failed", code: Int(task.terminationStatus))
        }
    }

    /// Every regular file in `directory`, recursively. `FileManager`'s enumerator
    /// already walks subdirectories, so each yielded path is only classified here.
    @discardableResult
    static func listAllFiles(in directory: URL) -> [URL] {
        var allFiles: [URL] = []
        let enumerator = FileManager.default.enumerator(atPath: directory.path)

        while let file = enumerator?.nextObject() as? String {
            let filePath = directory + [file]
            var isDirectory: ObjCBool = false

            if FileManager.default.fileExists(atPath: filePath.path, isDirectory: &isDirectory) {
                if !isDirectory.boolValue {
                    allFiles.append(filePath)
                }
            }
        }

        return allFiles
    }

    static func removeFiles(extensions: [String], currentDirectoryURL: URL) throws {
        for ext in extensions {
            let directoryContents = try FileManager.default.contentsOfDirectory(atPath: currentDirectoryURL.path)
            for item in directoryContents {
                if item.hasSuffix(ext) {
                    try FileManager.default.removeItem(at: currentDirectoryURL.appendingPathComponent(item))
                }
            }
        }
    }
}

extension URL {
    static var currentDirectory: URL {
        URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    }

    static func + (left: URL, right: String) -> URL {
        var url = left
        url.appendPathComponent(right)
        return url
    }

    static func + (left: URL, right: [String]) -> URL {
        var url = left
        right.forEach {
            url.appendPathComponent($0)
        }
        return url
    }
}
