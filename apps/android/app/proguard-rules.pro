# The engine's Kotlin classes are called from JNI and reflectively by name.
-keep class com.offlinetranslator.NativeBridge { *; }
-keep class com.offlinetranslator.** { *; }
-keepclasseswithmembernames class * { native <methods>; }
