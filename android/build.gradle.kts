// SPDX-License-Identifier: MIT
//
// Top-level build file.  The Android Gradle Plugin is declared once here and
// applied by :app; nothing else in the project needs a classpath entry.
plugins {
    id("com.android.application") version "8.7.3" apply false
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
