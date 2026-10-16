package com.edde746.mpv

/**
 * The value format an observed property is delivered in ([MpvPlayer.observeProperty]), and with it the
 * [PropertyChange] subtype its changes arrive as.
 */
enum class PropertyFormat(internal val nativeValue: Int) {
    None(0),
    String(1),
    Flag(3),
    Int64(4),
    Double(5)
}
