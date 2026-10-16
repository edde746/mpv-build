// Derived from libmpv-android's MPVLib.kt (MIT; Copyright (c) 2016 Ilya Zhuravlev, Copyright (c) 2016 sfan5);
// see platforms/android/lib/NOTICE.
package com.edde746.mpv

import android.content.Context
import android.os.Looper
import android.util.Log
import android.view.Surface
import java.util.concurrent.ConcurrentHashMap

/**
 * One native mpv session: an mpv handle with its own event thread and video output Surfaces, and one
 * library-owned thread that delivers its callbacks to a [Listener]. Create it with [create]; release it with
 * [close].
 *
 * **Threading.** Every method blocks the calling thread until mpv has answered, so none may run on the Android
 * main thread: [create], [attachSurfaces], [observeProperty], [setLogLevel] and [close] throw
 * [IllegalStateException] there, and the others block the same way without the check. Any other thread may call
 * any method, concurrently, including a [Listener] callback on the delivery thread.
 *
 * **Delivery.** mpv's event thread never waits for the listener and nothing is dropped while the player is open:
 * the JNI callbacks append to an unbounded per-session queue, and one thread per session, named
 * `mpv-listener-<session>`, calls the [Listener] in exactly mpv's order across property changes, events, log
 * lines ([MpvEvent.LogMessage]) and hooks. The lines explaining a failed open arrive before its
 * [MpvEvent.EndFile]. A slow listener delays later deliveries (and, for a hook, playback), never mpv itself. A
 * [Listener.onPropertyChange] or [Listener.onEvent] that throws is logged and delivery goes on. Callbacks may
 * begin before [create] returns: the event thread starts inside it.
 *
 * **Hooks.** Every session registers mpv's `on_preloaded` hook. It fires once per file, after the demuxer opened
 * it and before mpv creates the decoder, which makes it the place for per-file decode routing (`vo`, `hwdec`,
 * ...). [Listener.onHook] receives it on the delivery thread, and mpv holds playback until [continueHook] is
 * called with its id, from any thread, during or after the callback. The native continuation runs at most once
 * per id, whoever asks: a second [continueHook] is a no-op. With a `hookTimeoutMillis` greater than 0 the library
 * continues a hook itself once that long has passed since its [Listener.onHook] call began, and logs a warning;
 * a later [continueHook] for it is a no-op. An [Listener.onHook] that throws is logged and its hook continued at
 * once.
 *
 * **Lifetime.** Sessions are independent: each has its own mpv handle, event thread, delivery thread and
 * Surfaces, so a session whose [close] is stuck in a wedged decoder's teardown holds nothing the next one needs.
 * Calls on a closed player throw [IllegalStateException], except the getters (null), [continueHook] (no-op) and
 * [close] (no-op). The JNI layer refuses any call that names a session it has retired, so a retiring player's
 * in-flight work is inert against every other session.
 */
class MpvPlayer private constructor(
    /** Identity of the native session this player owns; also names its delivery thread. */
    val session: Long,
    private val listener: Listener,
    hookTimeoutMillis: Long,
) : AutoCloseable {

    /**
     * Receives one session's callbacks, all on its delivery thread (`mpv-listener-<session>`), one at a time,
     * in mpv's order. No callback starts once [close] has begun. A callback may call any [MpvPlayer] method,
     * [close] included.
     */
    interface Listener {
        /** An observed property changed ([observeProperty]). */
        fun onPropertyChange(change: PropertyChange) {}

        /** A lifecycle event or, as [MpvEvent.LogMessage], a log line at or above the requested level. */
        fun onEvent(event: MpvEvent) {}

        /**
         * mpv reached hook [name] (`on_preloaded`) and holds playback until [MpvPlayer.continueHook] is called
         * with [id]; the default continues at once. An implementation may keep the id and continue later, from
         * any thread, subject to the player's hook timeout. Throwing continues the hook at once.
         */
        fun onHook(player: MpvPlayer, name: String, id: Long) {
            player.continueHook(id)
        }
    }

    companion object {
        private const val TAG = "MpvPlayer"

        init {
            System.loadLibrary("mpv")
            System.loadLibrary("player")
        }

        private val sessions = ConcurrentHashMap<Long, MpvPlayer>()

        /**
         * Creates and initializes a native session, independent of every other session, including one that is
         * still terminating. Blocks through `mpv_initialize`; throws [IllegalStateException] on the Android main
         * thread.
         *
         * The library applies no option of its own: [configure] runs on this thread before `mpv_initialize` and
         * sets everything the session starts with. Log lines are requested at `warn` until
         * [MpvPlayerConfig.setLogLevel] or [setLogLevel] says otherwise.
         *
         * @param listener receives this session's callbacks; see [Listener]. Callbacks may begin before this
         *   returns.
         * @param hookTimeoutMillis how long after its [Listener.onHook] call began the library continues a hook
         *   itself (with a warning); 0, the default, never does.
         * @throws MpvException when mpv refuses an option or fails to initialize; nothing is left behind.
         */
        fun create(
            context: Context,
            listener: Listener,
            hookTimeoutMillis: Long = 0,
            configure: MpvPlayerConfig.() -> Unit = {},
        ): MpvPlayer {
            require(hookTimeoutMillis >= 0) { "hookTimeoutMillis must not be negative: $hookTimeoutMillis" }
            checkNotMainThread("MPV initialization")
            val player = MpvPlayer(nativeCreate(context.applicationContext), listener, hookTimeoutMillis)
            sessions[player.session] = player
            try {
                MpvPlayerConfig(player.session).apply(configure)
                val result = nativeInit(player.session)
                if (result < 0) throw MpvException("Failed to initialize mpv: error $result")
                return player
            } catch (e: Throwable) {
                player.close()
                throw e
            }
        }

        // JNI callbacks, called on a session's native event thread. Each names the session it originated from;
        // only the player registered for that session receives it, through its delivery queue, so none of them
        // blocks. A callback for a session that is not registered (closing, closed, or never published) is
        // dropped, and a hook among them is continued at once rather than left holding mpv.

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onPropertyChanged(session: Long, name: String, sourceId: Long, hasSourceId: Boolean) {
            sessions[session]?.dispatcher?.propertyChange(PropertyChange.None(name, sourceId.takeIf { hasSourceId }))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onPropertyChanged(session: Long, name: String, value: Boolean, sourceId: Long, hasSourceId: Boolean) {
            sessions[session]?.dispatcher?.propertyChange(PropertyChange.Flag(name, value, sourceId.takeIf { hasSourceId }))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onPropertyChanged(session: Long, name: String, value: Long, sourceId: Long, hasSourceId: Boolean) {
            sessions[session]?.dispatcher?.propertyChange(PropertyChange.Int64(name, value, sourceId.takeIf { hasSourceId }))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onPropertyChanged(session: Long, name: String, value: Double, sourceId: Long, hasSourceId: Boolean) {
            sessions[session]?.dispatcher?.propertyChange(PropertyChange.Double(name, value, sourceId.takeIf { hasSourceId }))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onPropertyChanged(session: Long, name: String, value: String, sourceId: Long, hasSourceId: Boolean) {
            sessions[session]?.dispatcher?.propertyChange(PropertyChange.Str(name, value, sourceId.takeIf { hasSourceId }))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onEvent(
            session: Long,
            eventId: Int,
            sourceId: Long,
            hasSourceId: Boolean,
            positionSeconds: Double,
            hasPositionSeconds: Boolean,
        ) {
            val player = sessions[session] ?: return
            val event = MpvEvent.fromId(
                eventId,
                sourceId.takeIf { hasSourceId },
                positionSeconds.takeIf { hasPositionSeconds && it.isFinite() },
            ) ?: return
            player.dispatcher.event(event)
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onEndFile(session: Long, reason: Int, sourceId: Long, hasSourceId: Boolean, error: Int, errorMessage: String?) {
            sessions[session]?.dispatcher?.event(
                MpvEvent.EndFile(
                    EndFileReason.fromId(reason),
                    sourceId.takeIf { hasSourceId },
                    MpvError.fromCode(error),
                    errorMessage?.takeIf { it.isNotBlank() },
                ),
            )
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onLogMessage(session: Long, prefix: String, level: Int, text: String) {
            val player = sessions[session] ?: return
            val logLevel = LogLevel.fromNative(level) ?: return
            player.dispatcher.event(MpvEvent.LogMessage(prefix, logLevel, text.trimEnd()))
        }

        /** JNI entry point of the glue (`event.cpp`); not for application use. */
        @JvmStatic
        fun onHook(session: Long, name: String, id: Long) {
            val player = sessions[session]
            if (player == null) {
                // Nobody will answer: release mpv rather than leave it waiting. The native side drops this if the
                // session has since been retired.
                nativeHookContinue(session, id)
                return
            }
            player.dispatcher.hook(name, id)
        }

        private fun checkNotMainThread(operation: String) {
            check(Looper.myLooper() != Looper.getMainLooper()) {
                "$operation must not run on the Android main thread"
            }
        }

        // JNI native declarations — private to avoid internal name mangling. Every entry after nativeCreate names
        // the session it acts for; the native side refuses a retired one.

        /** Publishes a new native session and returns its identity. */
        @JvmStatic private external fun nativeCreate(appctx: Context): Long

        /** 0 on success, otherwise a negative mpv error. */
        @JvmStatic private external fun nativeInit(session: Long): Int

        @JvmStatic private external fun nativeDestroy(session: Long)

        /** Negative mpv error, the playlist entry id a `loadfile` created, or 0 when the command returned none. */
        @JvmStatic private external fun nativeCommand(session: Long, cmd: Array<out String>): Long

        @JvmStatic private external fun nativeSetLogLevel(session: Long, level: String): Int

        @JvmStatic private external fun nativeHookContinue(session: Long, id: Long)

        @JvmStatic private external fun nativeSetOptionString(session: Long, name: String, value: String): Int

        @JvmStatic private external fun nativeAttachSurfaces(
            session: Long,
            surface: Surface,
            osdSurface: Surface?,
            videoGeneration: Long,
            osdGeneration: Long,
            videoOutput: String?,
        ): Int

        @JvmStatic private external fun nativeGetPropertyInt(session: Long, name: String): Int?

        @JvmStatic private external fun nativeGetPropertyDouble(session: Long, name: String): Double?

        @JvmStatic private external fun nativeGetPropertyBoolean(session: Long, name: String): Boolean?

        @JvmStatic private external fun nativeGetPropertyString(session: Long, name: String): String?

        @JvmStatic private external fun nativeSetPropertyInt(session: Long, name: String, value: Int): Int

        @JvmStatic private external fun nativeSetPropertyDouble(session: Long, name: String, value: Double): Int

        @JvmStatic private external fun nativeSetPropertyBoolean(session: Long, name: String, value: Boolean): Int

        @JvmStatic private external fun nativeSetPropertyString(session: Long, name: String, value: String): Int

        @JvmStatic private external fun nativeObserveProperty(session: Long, name: String, format: Int): Int

        internal fun setOptionString(session: Long, name: String, value: String): Int = nativeSetOptionString(session, name, value)

        internal fun requestLogMessages(session: Long, level: String) {
            checkNotMainThread("MPV log level change")
            val result = nativeSetLogLevel(session, level)
            if (result < 0) throw MpvException("Failed to set log level: error $result")
        }
    }

    private val closeLock = Any()

    @Volatile
    private var closed = false

    private val dispatcher = ListenerDispatcher(
        threadName = "mpv-listener-$session",
        target = object : ListenerDispatcher.Target {
            override fun onPropertyChange(change: PropertyChange) = listener.onPropertyChange(change)

            override fun onEvent(event: MpvEvent) = listener.onEvent(event)

            override fun onHook(name: String, id: Long) = listener.onHook(this@MpvPlayer, name, id)
        },
        hookTimeoutMillis = hookTimeoutMillis,
        continueNative = { id -> nativeHookContinue(session, id) },
        warn = { message, error -> Log.w(TAG, message, error) },
    )

    /**
     * Runs an mpv command (`mpv_command_ret`) and blocks until mpv has run it. `loadfile` returns the id of the
     * playlist entry it created — the `sourceId` of that entry's start-file, playback-restart and end-file
     * events; every other command returns null.
     *
     * @throws MpvException when mpv rejects the command: a rejected load never produces an entry, so the caller
     *   must not wait for one.
     * @throws IllegalStateException when the player is closed.
     */
    fun command(vararg args: String): Long? {
        checkNotClosed()
        val status = nativeCommand(session, args)
        if (status < 0) throw MpvException("Command '${args.firstOrNull() ?: ""}' failed: error $status")
        return if (status > 0) status else null
    }

    /**
     * Sets the minimum level of the log lines delivered as [MpvEvent.LogMessage] (`mpv_request_log_messages`:
     * `no`, `fatal`, `error`, `warn`, `info`, `v`, `debug`, `trace`). Throws [IllegalStateException] on the
     * Android main thread or when closed.
     *
     * @throws MpvException when mpv refuses the level.
     */
    fun setLogLevel(level: String) {
        checkNotClosed()
        requestLogMessages(session, level)
    }

    /**
     * Hands mpv's video output [surface] and, for `vo=mediacodec`, the subtitle/OSD plane [osdSurface] (null
     * retires the current one), and returns once mpv has retired the consumers they replace. Throws
     * [IllegalStateException] on the Android main thread or when closed.
     *
     * Generations name Surface lifetimes, not refresh requests: pass a new generation whenever a Surface is new,
     * even when the object is the same (a SurfaceView reuses one Surface object across recreations). A call
     * with the same objects and generations as the current ones changes nothing. There is no detach: to let go
     * of a Surface, attach another one (an app-owned placeholder, say) or [close] the player.
     *
     * @param videoOutput when non-null, also switches `vo` to it; [surface] and [videoGeneration] must then be
     *   the ones already attached.
     * @throws MpvException when mpv refuses the handoff (e.g. a [videoOutput] switch with a different video
     *   Surface); the previous Surfaces stay attached.
     */
    fun attachSurfaces(surface: Surface, osdSurface: Surface?, videoGeneration: Long, osdGeneration: Long, videoOutput: String? = null) {
        checkNotClosed()
        checkNotMainThread("MPV surface handoff")
        val result = nativeAttachSurfaces(session, surface, osdSurface, videoGeneration, osdGeneration, videoOutput)
        if (result < 0) throw MpvException("Failed to attach MPV surfaces: error $result")
    }

    /**
     * Continues hook [id] handed to [Listener.onHook], letting mpv proceed. Callable from any thread and
     * idempotent: the native continuation runs at most once per id, so a call after the hook timeout, after a
     * throwing [Listener.onHook], or after [close] already continued it is a no-op.
     */
    fun continueHook(id: Long) = dispatcher.continueHook(id)

    /** Reads [name] as an integer; null when it is unavailable, does not convert, or the player is closed. */
    fun getInt(name: String): Int? = if (closed) null else nativeGetPropertyInt(session, name)

    /** Reads [name] as a double; null when it is unavailable, does not convert, or the player is closed. */
    fun getDouble(name: String): Double? = if (closed) null else nativeGetPropertyDouble(session, name)

    /** Reads [name] as a flag; null when it is unavailable, does not convert, or the player is closed. */
    fun getFlag(name: String): Boolean? = if (closed) null else nativeGetPropertyBoolean(session, name)

    /** Reads [name] as a string; null when it is unavailable, does not convert, or the player is closed. */
    fun getString(name: String): String? = if (closed) null else nativeGetPropertyString(session, name)

    /**
     * Sets property [name] to an integer.
     *
     * @throws MpvException when mpv refuses the write.
     * @throws IllegalStateException when the player is closed.
     */
    fun setProperty(name: String, value: Int) {
        checkNotClosed()
        checkSet(name, nativeSetPropertyInt(session, name, value))
    }

    /**
     * Sets property [name] to a double.
     *
     * @throws MpvException when mpv refuses the write.
     * @throws IllegalStateException when the player is closed.
     */
    fun setProperty(name: String, value: Double) {
        checkNotClosed()
        checkSet(name, nativeSetPropertyDouble(session, name, value))
    }

    /**
     * Sets property [name] to a flag.
     *
     * @throws MpvException when mpv refuses the write.
     * @throws IllegalStateException when the player is closed.
     */
    fun setProperty(name: String, value: Boolean) {
        checkNotClosed()
        checkSet(name, nativeSetPropertyBoolean(session, name, value))
    }

    /**
     * Sets property [name] to a string.
     *
     * @throws MpvException when mpv refuses the write.
     * @throws IllegalStateException when the player is closed.
     */
    fun setProperty(name: String, value: String) {
        checkNotClosed()
        checkSet(name, nativeSetPropertyString(session, name, value))
    }

    /**
     * Observes [name]; its changes arrive through [Listener.onPropertyChange] as the [PropertyChange] subtype of
     * [format], starting with its current value. Throws [IllegalStateException] on the Android main thread or
     * when closed.
     *
     * @throws MpvException when mpv refuses: a property mpv does not know would otherwise report success while
     *   nothing is ever delivered for it.
     */
    fun observeProperty(name: String, format: PropertyFormat) {
        checkNotClosed()
        checkNotMainThread("MPV property observation")
        val result = nativeObserveProperty(session, name, format.nativeValue)
        if (result < 0) throw MpvException("Failed to observe property '$name': error $result")
    }

    /**
     * Destroys the session. Idempotent; callable from any thread but the Android main thread (which throws
     * [IllegalStateException]), including from inside a [Listener] callback.
     *
     * In order: no [Listener] call starts any more and queued deliveries are discarded; every hook still
     * unanswered, delivered or queued, is continued; then the native session is destroyed, which blocks through
     * decoder and video output teardown and, on a wedged decoder, may never return. No other session waits on
     * it. Callbacks mpv raises meanwhile are dropped, and hooks among them continued at once. The delivery thread
     * ends as soon as a [Listener] call in progress returns; close never waits for it, so calling it from the
     * delivery thread cannot deadlock.
     */
    override fun close() {
        synchronized(closeLock) {
            if (closed) return
            checkNotMainThread("MPV destruction")
            closed = true
            sessions.remove(session, this)
        }
        dispatcher.close()
        nativeDestroy(session)
    }

    private fun checkSet(name: String, result: Int) {
        if (result < 0) throw MpvException("Failed to set property '$name': error $result")
    }

    private fun checkNotClosed() {
        check(!closed) { "MpvPlayer has been closed" }
    }
}
