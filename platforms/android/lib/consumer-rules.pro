# JNI exports bind by name (Java_com_edde746_mpv_MpvPlayer_native*), and libavcodec binds
# MediaCodecCallbackBridge's natives by name with RegisterNatives; keep the names stable.
-keepclasseswithmembernames class com.edde746.mpv.* {
    native <methods>;
}

# jni_utils.cpp caches MpvPlayer with FindClass and resolves these static callbacks with
# GetStaticMethodID on the first nativeCreate; event.cpp calls them on the native event thread.
# R8 sees no reference to the class name or the member names, so both must stay alive and
# un-renamed. Match callback names, return type and static access without duplicating JNI
# argument descriptors here; native initialization resolves every exact descriptor.
-keep class com.edde746.mpv.MpvPlayer {
    public static void onPropertyChanged(...);
    public static void onEvent(...);
    public static void onEndFile(...);
    public static void onLogMessage(...);
    public static void onHook(...);
}

# main.cpp resolves MediaCodecCallbackBridge with FindClass and hands it to libavcodec
# (av_jni_set_mediacodec_callback_class), which resolves the constructor, handler() and
# release() with GetMethodID and binds the native methods with RegisterNatives. No Kotlin or
# Java code references the class, so R8 would drop it and the Java MediaCodec wrapper would
# silently fall back to synchronous decoding with no rendered-frame feedback.
-keep class com.edde746.mpv.MediaCodecCallbackBridge {
    <init>(long);
    android.os.Handler handler();
    void release();
    native <methods>;
}
