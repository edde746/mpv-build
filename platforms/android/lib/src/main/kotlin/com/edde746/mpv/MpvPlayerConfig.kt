package com.edde746.mpv

/**
 * Configuration of one native session before `mpv_initialize`, handed to the `configure` block of
 * [MpvPlayer.create] on the creating thread. Every write names that session. The library sets no option of its
 * own: what the session starts with is mpv's defaults plus what this block sets.
 */
class MpvPlayerConfig internal constructor(private val session: Long) {
    /**
     * Sets an mpv option (`mpv_set_option_string`), as on mpv's command line without the leading dashes.
     *
     * @throws MpvException when mpv refuses the option or its value.
     */
    fun setOption(name: String, value: String) {
        val result = MpvPlayer.setOptionString(session, name, value)
        if (result < 0) {
            throw MpvException("Failed to set option '$name' to '$value': error $result")
        }
    }

    /**
     * Sets the minimum level of the log lines delivered as [MpvEvent.LogMessage] (`mpv_request_log_messages`:
     * `no`, `fatal`, `error`, `warn`, `info`, `v`, `debug`, `trace`). A new session starts at `warn`.
     *
     * @throws MpvException when mpv refuses the level.
     */
    fun setLogLevel(level: String) {
        MpvPlayer.requestLogMessages(session, level)
    }
}
