// SPDX-License-Identifier: MIT
//
// Deliberately no pluginManagement repositories block beyond what is needed,
// and no dependencyResolutionManagement surprises: RVM has exactly one module
// and zero third-party Java dependencies.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

rootProject.name = "rvm"
include(":app")
