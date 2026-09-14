# The engine's Kotlin classes are called from JNI and reflectively by name.
-keep class com.offlinetranslator.NativeBridge { *; }
-keep class com.offlinetranslator.** { *; }
-keepclasseswithmembernames class * { native <methods>; }

# tesseract4android's Java classes are reached from its native code by name.
-keep class com.googlecode.tesseract.android.** { *; }
-keep class com.googlecode.leptonica.android.** { *; }
