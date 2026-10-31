// The mpv-android library (docs/android.md): one project, the library itself.
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
