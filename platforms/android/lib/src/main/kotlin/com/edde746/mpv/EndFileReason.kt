package com.edde746.mpv

/** Why mpv ended a playlist entry (`mpv_end_file_reason`, client.h). */
enum class EndFileReason(val id: Int) {
    Eof(0),
    Stop(2),
    Quit(3),
    Error(4),
    Redirect(5);

    companion object {
        /** Null for a reason this enum does not model. */
        fun fromId(id: Int): EndFileReason? = entries.find { it.id == id }
    }
}
