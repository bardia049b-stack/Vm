# SPDX-License-Identifier: MIT
#
# RVM's Java surface is tiny and almost all of it is called from C through JNI,
# so the members the native side reaches must survive shrinking and renaming.

-keepclasseswithmembernames class * {
    native <methods>;
}

# The JNI callback targets, looked up by name from rvm_jni.c.
-keep class dev.rvm.app.RvmNative {
    *;
}
-keepclassmembers class dev.rvm.app.RvmNative {
    void onConsoleOutput(byte[], int);
    void onFrame(byte[], int, int, int);
    void onVmExit(int);
}

# Custom Views inflated from XML need their (Context, AttributeSet) ctor.
-keepclasseswithmembers class dev.rvm.app.RvmView { <init>(...); }
-keepclasseswithmembers class dev.rvm.app.GfxView { <init>(...); }
-keepclasseswithmembers class dev.rvm.app.KeyBar  { <init>(...); }

-dontwarn java.lang.invoke.StringConcatFactory
