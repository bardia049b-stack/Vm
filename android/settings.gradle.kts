// SPDX-License-Identifier: MIT
//
// RVM has exactly one module and zero third-party Java dependencies, but the
// Android Gradle Plugin still resolves aapt2 as an external module, so the
// project-level repositories below are not optional: without them the build
// dies with "Cannot resolve com.android.tools.build:aapt2 ... because no
// repositories are defined".

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

rootProject.name = "rvm"
include(":app")
