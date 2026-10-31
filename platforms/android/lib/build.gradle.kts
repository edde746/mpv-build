// The mpv-android library (docs/android.md): the JNI glue libplayer.so and its Kotlin API
// (com.edde746.mpv), packed with the libmpv-android tarballs' native set into one self-contained AAR.
//
// The tarballs come from -Pmpv.tarballs=<dir>, one libmpv-android-[<key>-]<abi>.tar.gz per ABI; build-aar.sh
// fetches them (or takes local ones) and names the result. CMake links libplayer.so against the tarball's
// libmpv.so and libavcodec.so and compiles against its include/ (mpv's and the patched FFmpeg's headers);
// AGP packages those two with libplayer.so, and every other .so of the tarball, libc++_shared.so included,
// is staged below as a jniLibs source. The tarballs' licence texts, with the library's own, become the AAR's
// assets/licenses/<component>/.
import org.gradle.api.file.FileSystemOperations
import javax.inject.Inject

plugins {
    id("com.android.library") version "9.3.1"
    // AGP 9 compiles Kotlin itself; this puts the Kotlin compiler both consumers use on its classpath.
    id("org.jetbrains.kotlin.android") version "2.4.10" apply false
}

/** The group.json variants of libmpv-android, in its order. */
val abis = listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")

/** The NDK the tarballs were built with: toolchain/android.txt, which every android content key hashes. */
val pinnedNdk: String = rootDir.resolve("../../../toolchain/android.txt").readLines()
    .single { it.startsWith("ndk=") }.substringAfter('=')

fun tarball(abi: String): File {
    val dir = providers.gradleProperty("mpv.tarballs").orNull?.let(::file)
        ?: throw GradleException(
            "pass -Pmpv.tarballs=<directory with the libmpv-android tarballs>; " +
                "platforms/android/lib/build-aar.sh fetches the pinned ones",
        )
    val name = Regex("libmpv-android-(?:[0-9a-f]{12}-)?${Regex.escape(abi)}\\.tar\\.gz")
    val matches = dir.listFiles().orEmpty().filter { name.matches(it.name) }
    return matches.singleOrNull()
        ?: throw GradleException("$dir: expected one libmpv-android tarball for $abi, found ${matches.map { it.name }}")
}

val mpvNatives = layout.buildDirectory.dir("mpv")

/** build/mpv/<abi>/{lib,include}: the tarballs unpacked. */
val extractMpvNatives = tasks.register<Sync>("extractMpvNatives") {
    for (abi in abis) {
        from(provider { tarTree(resources.gzip(tarball(abi))) }) { into(abi) }
    }
    into(mpvNatives)
}

/** Copies the tarball libraries CMake does not link into a jniLibs tree (`<abi>/lib*.so`). */
abstract class StageMpvJniLibs : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val nativeRoot: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Inject
    abstract val files: FileSystemOperations

    @TaskAction
    fun stage() {
        files.sync {
            from(nativeRoot) {
                include("*/lib/*.so")
                exclude("*/lib/libmpv.so", "*/lib/libavcodec.so")
                eachFile { relativePath = RelativePath(true, relativePath.segments.first(), name) }
            }
            includeEmptyDirs = false
            into(outputDir)
        }
    }
}

val stageMpvJniLibs = tasks.register<StageMpvJniLibs>("stageMpvJniLibs") {
    dependsOn(extractMpvNatives)
    nativeRoot.set(mpvNatives)
    outputDir.set(layout.buildDirectory.dir("mpv-jnilibs"))
}

/**
 * assets/licenses/<component>/: the licence texts the tarballs carry for everything linked into their libraries,
 * which every ABI's tarball must carry alike, and mpv-android/, this library's own: the NOTICE of the code it
 * derives from and mpv-build's licence.
 */
abstract class StageMpvLicenses : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val nativeRoot: DirectoryProperty

    /** The ABIs in group.json order; the first one's texts are shipped. */
    @get:Input
    abstract val variants: ListProperty<String>

    @get:InputFiles
    @get:PathSensitive(PathSensitivity.NAME_ONLY)
    abstract val ownTexts: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun stage() {
        fun texts(abi: String): Map<String, ByteArray> {
            val dir = nativeRoot.get().asFile.resolve("$abi/licenses")
            return dir.walkTopDown().filter { it.isFile }
                .associate { it.relativeTo(dir).invariantSeparatorsPath to it.readBytes() }
        }
        val (first, others) = variants.get().let { it.first() to it.drop(1) }
        val shipped = texts(first)
        if (shipped.isEmpty()) throw GradleException("the $first tarball carries no licenses/: it predates them")
        for (abi in others) {
            val texts = texts(abi)
            if (texts.keys != shipped.keys || texts.any { (path, bytes) -> !bytes.contentEquals(shipped[path]) }) {
                throw GradleException("the $abi tarball's licenses/ differ from the $first one's")
            }
        }
        val out = outputDir.get().asFile.resolve("licenses")
        out.deleteRecursively()
        for ((path, bytes) in shipped) out.resolve(path).apply { parentFile.mkdirs() }.writeBytes(bytes)
        for (text in ownTexts) text.copyTo(out.resolve("mpv-android/${text.name}"))
    }
}

val stageMpvLicenses = tasks.register<StageMpvLicenses>("stageMpvLicenses") {
    dependsOn(extractMpvNatives)
    nativeRoot.set(mpvNatives)
    variants.set(abis)
    ownTexts.from("NOTICE", "../../../LICENSE")
    outputDir.set(layout.buildDirectory.dir("mpv-assets"))
}

android {
    namespace = "com.edde746.mpv"
    compileSdk = 36
    ndkVersion = pinnedNdk

    defaultConfig {
        // The floor libmpv-android is built for (platforms/android/include/loadarch.sh).
        minSdk = 25
        consumerProguardFiles("consumer-rules.pro")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += abis }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DMPV_NATIVE_ROOT=${mpvNatives.get().asFile.absolutePath}",
                )
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    packaging {
        // The tarball's libc++_shared.so (the one libmpv was linked against) and the NDK's, which CMake adds for
        // c++_shared: the same NDK, the same bytes. jniLibs merge first, so the tarball's is the one packed.
        jniLibs { pickFirsts += "lib/*/libc++_shared.so" }
    }
}

androidComponents {
    onVariants { variant ->
        variant.sources.jniLibs?.addGeneratedSourceDirectory(stageMpvJniLibs, StageMpvJniLibs::outputDir)
        variant.sources.assets?.addGeneratedSourceDirectory(stageMpvLicenses, StageMpvLicenses::outputDir)
    }
}

// CMake links the imported libmpv.so/libavcodec.so and reads the tarball's headers, so they must exist before it
// configures.
tasks.matching { it.name.contains("CMake") }.configureEach { dependsOn(extractMpvNatives) }

// Each JVM test's outcome in the build log (CI's only record of them).
tasks.withType<Test>().configureEach {
    testLogging { events("passed", "skipped", "failed") }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    // MediaCodecQueryTest: MediaCodecInfo on the API levels below 29, where isHardwareAccelerated() does not exist.
    testImplementation("org.robolectric:robolectric:4.16.1")
    androidTestImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test:core:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
}
