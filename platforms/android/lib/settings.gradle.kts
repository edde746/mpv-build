// The mpv-android library (README.md, Android library): one project, the library itself.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

// Names the AAR: build/outputs/aar/mpv-android-release.aar.
rootProject.name = "mpv-android"
