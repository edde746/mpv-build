package com.edde746.mpv

import java.util.ArrayDeque
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * One session's delivery queue, the thread that drains it into a [Target], and the bookkeeping that answers
 * every hook exactly once. [MpvPlayer] wires it to the JNI callbacks, `nativeHookContinue` and `android.util.Log`;
 * nothing here touches an Android API or a native method, so it runs on a plain JVM.
 *
 * - The producer side ([propertyChange], [event], [hook]) never waits for the consumer: it appends to an unbounded
 *   queue under a lock that is only ever held for queue and map updates, never across a [Target] call.
 * - One thread, named [threadName], calls [target] in exactly the order the producer appended.
 * - Every hook handed to [hook] is answered through [continueNative] exactly once: by [continueHook], by the
 *   timeout, after a [Target.onHook] that threw, by [close], or at once when it arrives after [close].
 * - [close] never waits for the thread, so it may run on the thread itself (inside a [Target] call).
 *
 * @param hookTimeoutMillis when > 0, a hook not answered this long after its [Target.onHook] call began is
 *   answered by the dispatcher and reported through [warn]; 0 disables the timeout.
 * @param continueNative answers a hook to mpv; called at most once per hook id, outside the dispatcher's lock.
 * @param warn reports a listener failure or an expired hook; called outside the dispatcher's lock.
 */
internal class ListenerDispatcher(
    threadName: String,
    private val target: Target,
    private val hookTimeoutMillis: Long,
    private val continueNative: (id: Long) -> Unit,
    private val warn: (message: String, error: Throwable?) -> Unit,
) {
    /** What the delivery thread calls, in order. */
    interface Target {
        fun onPropertyChange(change: PropertyChange)

        fun onEvent(event: MpvEvent)

        fun onHook(name: String, id: Long)
    }

    private class Hook(val name: String, val id: Long)

    private val lock = ReentrantLock()
    private val available = lock.newCondition()

    /** [PropertyChange], [MpvEvent] or [Hook], in the producer's order. Guarded by [lock]. */
    private val queue = ArrayDeque<Any>()

    /**
     * Every hook handed over and not yet answered, queued or delivered, in mpv's order; the value is its pending
     * timeout once delivered. Guarded by [lock]. Removal is what entitles a caller to answer the hook.
     */
    private val unanswered = LinkedHashMap<Long, ScheduledFuture<*>?>()

    /** Set once, under [lock]; read unlocked only as a last check before a [Target] call. */
    @Volatile
    private var closed = false

    private val thread = Thread({ drain() }, threadName).apply { isDaemon = true }

    init {
        require(hookTimeoutMillis >= 0) { "hookTimeoutMillis must not be negative: $hookTimeoutMillis" }
        thread.start()
    }

    fun propertyChange(change: PropertyChange) = enqueue(change)

    fun event(event: MpvEvent) = enqueue(event)

    /** Queues a hook for [Target.onHook]; after [close] it is answered at once instead. */
    fun hook(name: String, id: Long) {
        val queued = lock.withLock {
            if (closed) {
                false
            } else {
                unanswered[id] = null
                queue.addLast(Hook(name, id))
                available.signal()
                true
            }
        }
        if (!queued) continueNative(id)
    }

    /** Answers hook [id] unless it was already answered (or never handed over); safe from any thread. */
    fun continueHook(id: Long) {
        if (claim(id)) continueNative(id)
    }

    /**
     * Stops delivery: no [Target] call starts after this has marked the dispatcher closed, queued deliveries
     * are discarded, every unanswered hook is answered before this returns, and the thread ends once a
     * [Target] call already in progress returns. Idempotent; never waits for the thread.
     */
    fun close() {
        val pending = lock.withLock {
            if (closed) return
            closed = true
            queue.clear()
            for (timeout in unanswered.values) timeout?.cancel(false)
            val ids = unanswered.keys.toLongArray()
            unanswered.clear()
            available.signalAll()
            ids
        }
        for (id in pending) continueNative(id)
    }

    private fun enqueue(item: Any) {
        lock.withLock {
            if (closed) return
            queue.addLast(item)
            available.signal()
        }
    }

    /** True for exactly one caller per handed-over hook: the one that is to answer it. */
    private fun claim(id: Long): Boolean {
        lock.withLock {
            if (!unanswered.containsKey(id)) return false
            unanswered.remove(id)?.cancel(false)
            return true
        }
    }

    private fun drain() {
        while (true) {
            val next = lock.withLock {
                while (queue.isEmpty() && !closed) available.awaitUninterruptibly()
                if (closed) return
                queue.removeFirst()
            }
            if (closed) return
            when (next) {
                is Hook -> deliverHook(next)
                is PropertyChange -> try {
                    target.onPropertyChange(next)
                } catch (e: Exception) {
                    warn("Listener.onPropertyChange(${next.name}) failed", e)
                }
                is MpvEvent -> try {
                    target.onEvent(next)
                } catch (e: Exception) {
                    warn("Listener.onEvent($next) failed", e)
                }
            }
        }
    }

    private fun deliverHook(hook: Hook) {
        if (hookTimeoutMillis > 0) {
            lock.withLock {
                if (unanswered.containsKey(hook.id)) {
                    val timeout = Runnable { expire(hook) }
                    unanswered[hook.id] = timeouts.schedule(timeout, hookTimeoutMillis, TimeUnit.MILLISECONDS)
                }
            }
        }
        try {
            target.onHook(hook.name, hook.id)
        } catch (e: Exception) {
            warn("Listener.onHook(${hook.name}) failed; continuing playback", e)
            continueHook(hook.id)
        }
    }

    private fun expire(hook: Hook) {
        if (!claim(hook.id)) return
        warn("Hook ${hook.name} was not continued within $hookTimeoutMillis ms; continuing playback", null)
        continueNative(hook.id)
    }

    private companion object {
        /**
         * Fires hook timeouts for every session; created on first use and idle-free: its one daemon thread
         * exits after a quiet spell and comes back with the next timeout.
         */
        val timeouts: ScheduledThreadPoolExecutor by lazy {
            ScheduledThreadPoolExecutor(1) { task -> Thread(task, "mpv-hook-timeout").apply { isDaemon = true } }.apply {
                removeOnCancelPolicy = true
                setKeepAliveTime(10, TimeUnit.SECONDS)
                allowCoreThreadTimeOut(true)
            }
        }
    }
}
