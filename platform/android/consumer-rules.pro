# Keep the JNI surface: native code looks these up by name.
-keep class com.offlinetranslator.NativeBridge { *; }
-keep class com.offlinetranslator.NativeBridge$ProgressListener { *; }
