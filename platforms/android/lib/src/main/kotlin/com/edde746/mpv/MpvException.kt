package com.edde746.mpv

/** mpv refused a call; the message names the call and the negative `mpv_error` code. */
class MpvException(message: String) : RuntimeException(message)
