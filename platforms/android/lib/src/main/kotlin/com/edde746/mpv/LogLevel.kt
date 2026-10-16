package com.edde746.mpv

/** Level of an mpv log line (`mpv_log_level`, client.h). */
enum class LogLevel(internal val nativeValue: Int) {
    Fatal(10),
    Error(20),
    Warn(30),
    Info(40),
    Verbose(50),
    Debug(60),
    Trace(70);

    companion object {
        internal fun fromNative(value: Int): LogLevel? = entries.find { it.nativeValue == value }
    }
}
