package com.edde746.mpv

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class ListenerDispatcherTest {
    private val calls = CopyOnWriteArrayList<String>()
    private val delivered = Semaphore(0)
    private val continued = CopyOnWriteArrayList<Long>()
    private val continuations = Semaphore(0)
    private val warnings = CopyOnWriteArrayList<Pair<String, Throwable?>>()
    private val dispatchers = CopyOnWriteArrayList<ListenerDispatcher>()

    /** Records every call; a hook stays unanswered unless a test answers it. */
    private open inner class Recording : ListenerDispatcher.Target {
        override fun onPropertyChange(change: PropertyChange) = record("property ${change.name}")

        override fun onEvent(event: MpvEvent) = record(describe(event))

        override fun onHook(name: String, id: Long) = record("hook $name $id")
    }

    @After
    fun closeAll() {
        for (dispatcher in dispatchers) dispatcher.close()
    }

    @Test
    fun deliversEveryKindInTheOrderItArrived() {
        val dispatcher = newDispatcher(Recording())
        dispatcher.event(MpvEvent.StartFile(7))
        dispatcher.propertyChange(PropertyChange.Flag("seekable", true, 7))
        dispatcher.event(MpvEvent.LogMessage("ffmpeg", LogLevel.Warn, "http: HTTP error 404 Not Found"))
        dispatcher.hook("on_preloaded", 17)
        dispatcher.propertyChange(PropertyChange.Int64("playlist-count", 1, 7))
        dispatcher.event(MpvEvent.EndFile(EndFileReason.Error, 7, MpvError.LoadingFailed, "loading failed"))
        awaitCalls(6)
        assertEquals(
            listOf(
                "start-file 7",
                "property seekable",
                "log http: HTTP error 404 Not Found",
                "hook on_preloaded 17",
                "property playlist-count",
                "end-file 7 Error",
            ),
            calls.toList(),
        )
    }

    @Test
    fun theProducerNeverWaitsForABlockedListener() {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val dispatcher = newDispatcher(object : Recording() {
            override fun onEvent(event: MpvEvent) {
                if (event is MpvEvent.StartFile) {
                    entered.countDown()
                    release.await()
                }
                super.onEvent(event)
            }
        })
        dispatcher.event(MpvEvent.StartFile(1))
        assertTrue("the listener was never called", entered.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))

        val produced = CountDownLatch(1)
        val producer = Thread {
            repeat(10_000) { dispatcher.propertyChange(PropertyChange.Int64("n", it.toLong(), 1)) }
            dispatcher.event(MpvEvent.LogMessage("cplayer", LogLevel.Info, "still producing"))
            dispatcher.hook("on_preloaded", 2)
            produced.countDown()
        }
        try {
            producer.start()
            assertTrue("the producer waited for the blocked listener", produced.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))
            assertEquals("a delivery overtook the blocked one", 0, calls.size)
        } finally {
            release.countDown()
        }
        awaitCalls(1 + 10_000 + 2)
        assertEquals("hook on_preloaded 2", calls.last())
    }

    @Test
    fun aBurstFromSeveralProducersLosesNothingAndKeepsEachProducersOrder() {
        val producers = 4
        val perProducer = 25_000
        val seen = ArrayList<Long>(producers * perProducer)
        val all = CountDownLatch(1)
        val dispatcher = newDispatcher(object : Recording() {
            override fun onPropertyChange(change: PropertyChange) {
                seen += (change as PropertyChange.Int64).value
                if (seen.size == producers * perProducer) all.countDown()
            }
        })
        val start = CountDownLatch(1)
        val threads = List(producers) { producer ->
            Thread {
                start.await()
                for (i in 0 until perProducer) {
                    dispatcher.propertyChange(PropertyChange.Int64("n", producer * 1_000_000L + i, null))
                }
            }.apply { start() }
        }
        start.countDown()
        threads.forEach { it.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS)) }
        assertTrue("a delivery was lost: ${seen.size} arrived", all.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))
        for (producer in 0 until producers) {
            val own = seen.filter { it / 1_000_000L == producer.toLong() }.map { it % 1_000_000L }
            assertEquals("producer $producer's deliveries were lost or reordered", (0L until perProducer).toList(), own)
        }
    }

    @Test
    fun continueHookContinuesExactlyOnce() {
        val dispatcher = newDispatcher(Recording())
        dispatcher.hook("on_preloaded", 17)
        awaitCalls(1)

        val start = CountDownLatch(1)
        val callers = List(8) {
            Thread {
                start.await()
                dispatcher.continueHook(17)
            }.apply { start() }
        }
        start.countDown()
        for (caller in callers) {
            caller.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS))
            assertFalse(caller.isAlive)
        }
        dispatcher.continueHook(17)
        dispatcher.continueHook(99)
        assertEquals(listOf(17L), continued.toList())

        dispatcher.close()
        assertEquals("close continued an answered hook again", listOf(17L), continued.toList())
    }

    @Test
    fun aTimedOutHookIsContinuedOnceAndALateContinueIsANoOp() {
        val timeoutMillis = 50L
        val dispatcher = newDispatcher(Recording(), hookTimeoutMillis = timeoutMillis)
        val before = System.nanoTime()
        dispatcher.hook("on_preloaded", 17)
        assertTrue("the timeout never continued the hook", continuations.tryAcquire(TIMEOUT_SECONDS, TimeUnit.SECONDS))
        val elapsedNanos = System.nanoTime() - before
        assertTrue(
            "continued after ${TimeUnit.NANOSECONDS.toMillis(elapsedNanos)} ms, before the $timeoutMillis ms timeout",
            elapsedNanos >= TimeUnit.MILLISECONDS.toNanos(timeoutMillis),
        )
        assertEquals(listOf("hook on_preloaded 17"), calls.toList())
        assertEquals(listOf(17L), continued.toList())
        val warning = warnings.single()
        assertTrue(warning.first, warning.first.contains("on_preloaded"))

        dispatcher.continueHook(17)
        dispatcher.close()
        assertEquals("a late continueHook continued the hook again", listOf(17L), continued.toList())
    }

    @Test
    fun anAnsweredHookIsNotContinuedAgainByItsTimeout() {
        lateinit var dispatcher: ListenerDispatcher
        dispatcher = newDispatcher(object : Recording() {
            override fun onHook(name: String, id: Long) {
                super.onHook(name, id)
                dispatcher.continueHook(id)
            }
        }, hookTimeoutMillis = 20)
        dispatcher.hook("on_preloaded", 17)
        assertTrue(continuations.tryAcquire(TIMEOUT_SECONDS, TimeUnit.SECONDS))
        // Past the timeout: had it not been cancelled, it would have fired by now.
        Thread.sleep(100)
        assertEquals(listOf(17L), continued.toList())
        assertTrue(warnings.toString(), warnings.isEmpty())
    }

    @Test
    fun withoutATimeoutAHookWaitsForItsAnswer() {
        val dispatcher = newDispatcher(Recording())
        dispatcher.hook("on_preloaded", 17)
        dispatcher.event(MpvEvent.FileLoaded(1))
        // The next delivery ran, so onHook has returned with the hook unanswered.
        awaitCalls(2)
        Thread.sleep(100)
        assertTrue("an unanswered hook was continued without a timeout", continued.isEmpty())
        assertTrue(warnings.toString(), warnings.isEmpty())

        dispatcher.continueHook(17)
        assertEquals(listOf(17L), continued.toList())
    }

    @Test
    fun aThrowingOnHookIsContinuedAtOnce() {
        val failure = IllegalStateException("routing failed")
        val continuedBeforeNext = AtomicReference<List<Long>>()
        val dispatcher = newDispatcher(object : Recording() {
            override fun onHook(name: String, id: Long) {
                super.onHook(name, id)
                throw failure
            }

            override fun onEvent(event: MpvEvent) {
                continuedBeforeNext.set(continued.toList())
                super.onEvent(event)
            }
        }, hookTimeoutMillis = 60_000)
        dispatcher.hook("on_preloaded", 17)
        dispatcher.event(MpvEvent.FileLoaded(1))
        awaitCalls(2)
        assertEquals("the hook was not continued before the next delivery", listOf(17L), continuedBeforeNext.get())
        assertSame(failure, warnings.single().second)

        dispatcher.continueHook(17)
        assertEquals(listOf(17L), continued.toList())
    }

    @Test
    fun closeFromInsideACallbackStopsDeliveryWithoutDeadlock() {
        val name = threadName()
        val closedInside = CountDownLatch(1)
        lateinit var dispatcher: ListenerDispatcher
        dispatcher = newDispatcher(object : Recording() {
            override fun onEvent(event: MpvEvent) {
                super.onEvent(event)
                if (event is MpvEvent.EndFile) {
                    dispatcher.close()
                    dispatcher.close()
                    closedInside.countDown()
                }
            }
        }, name = name)
        val thread = threadNamed(name)

        dispatcher.hook("on_preloaded", 5)
        dispatcher.event(MpvEvent.EndFile(EndFileReason.Eof, 1))
        dispatcher.event(MpvEvent.StartFile(2))
        dispatcher.propertyChange(PropertyChange.None("track-list", 2))
        assertTrue("close deadlocked inside the callback", closedInside.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))
        thread.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS))
        assertFalse("the delivery thread outlived close", thread.isAlive)

        dispatcher.event(MpvEvent.FileLoaded(2))
        dispatcher.hook("on_preloaded", 6)
        assertEquals(listOf("hook on_preloaded 5", "end-file 1 Eof"), calls.toList())
        assertEquals(listOf(5L, 6L), continued.toList())
    }

    @Test
    fun closeContinuesEveryUnansweredHookAndStopsDelivery() {
        val name = threadName()
        val inHook = CountDownLatch(1)
        val release = CountDownLatch(1)
        val dispatcher = newDispatcher(object : Recording() {
            override fun onHook(name: String, id: Long) {
                super.onHook(name, id)
                inHook.countDown()
                release.await()
            }
        }, hookTimeoutMillis = 60_000, name = name)
        val thread = threadNamed(name)
        try {
            dispatcher.hook("on_preloaded", 1)
            assertTrue(inHook.await(TIMEOUT_SECONDS, TimeUnit.SECONDS))
            dispatcher.hook("on_preloaded", 2)
            dispatcher.event(MpvEvent.FileLoaded(1))

            dispatcher.close()
            assertEquals("close left a delivered or queued hook unanswered", listOf(1L, 2L), continued.toList())

            dispatcher.hook("on_preloaded", 3)
            dispatcher.event(MpvEvent.PlaybackRestart(1, 0.0))
            assertEquals("a hook arriving after close was not continued at once", listOf(1L, 2L, 3L), continued.toList())
        } finally {
            release.countDown()
        }
        thread.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS))
        assertFalse("the delivery thread outlived close", thread.isAlive)

        dispatcher.continueHook(1)
        dispatcher.continueHook(2)
        dispatcher.continueHook(3)
        assertEquals(listOf(1L, 2L, 3L), continued.toList())
        assertEquals("a delivery started after close", listOf("hook on_preloaded 1"), calls.toList())
        assertTrue(warnings.toString(), warnings.isEmpty())
    }

    private fun newDispatcher(
        target: ListenerDispatcher.Target,
        hookTimeoutMillis: Long = 0,
        name: String = threadName(),
    ): ListenerDispatcher = ListenerDispatcher(
        threadName = name,
        target = target,
        hookTimeoutMillis = hookTimeoutMillis,
        continueNative = { id ->
            continued += id
            continuations.release()
        },
        warn = { message, error -> warnings += message to error },
    ).also { dispatchers += it }

    private fun record(call: String) {
        calls += call
        delivered.release()
    }

    private fun awaitCalls(count: Int) {
        assertTrue(
            "only ${calls.size} of $count deliveries arrived: $calls",
            delivered.tryAcquire(count, TIMEOUT_SECONDS, TimeUnit.SECONDS),
        )
    }

    private fun describe(event: MpvEvent): String = when (event) {
        is MpvEvent.StartFile -> "start-file ${event.sourceId}"
        is MpvEvent.EndFile -> "end-file ${event.sourceId} ${event.reason}"
        is MpvEvent.FileLoaded -> "file-loaded ${event.sourceId}"
        is MpvEvent.PlaybackRestart -> "playback-restart ${event.sourceId}"
        is MpvEvent.LogMessage -> "log ${event.text}"
    }

    private fun threadNamed(name: String): Thread = Thread.getAllStackTraces().keys.single { it.name == name }

    private companion object {
        const val TIMEOUT_SECONDS = 5L
        val serial = AtomicInteger()

        fun threadName(): String = "test-listener-${serial.incrementAndGet()}"
    }
}
