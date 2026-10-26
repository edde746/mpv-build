package com.edde746.mpv

import android.graphics.ImageFormat
import android.graphics.Bitmap
import android.graphics.Color
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.PixelCopy
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * [MpvPlayer] against the real libmpv on a device or emulator: a session is created, loads the test media,
 * reports its lifecycle through the [MpvPlayer.Listener] on the delivery thread, renders into video Surfaces that
 * are attached, replaced and recreated under new generations, delivers an observed property, and on close stops
 * delivering and ends its delivery thread. The Surfaces are ImageReader-backed, so no window is needed; audio
 * goes to `ao=null`.
 *
 * The media is required as the instrumentation argument `mediaUrl`, any URL or path mpv can open that has a
 * video track, e.g. `adb shell am instrument -w -e mediaUrl http://127.0.0.1:8765/sample.mkv ...` (with
 * `adb reverse tcp:8765 tcp:8765`), or through Gradle with
 * `-Pandroid.testInstrumentationRunnerArguments.mediaUrl=<url> connectedDebugAndroidTest`.
 */
@RunWith(AndroidJUnit4::class)
class MpvPlayerDeviceTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private lateinit var mediaUrl: String
    private val sinks = mutableListOf<FrameSink>()
    private val players = mutableListOf<MpvPlayer>()

    @Before
    fun requireMedia() {
        mediaUrl = InstrumentationRegistry.getArguments().getString("mediaUrl")
            ?: throw AssertionError("Pass the test media as the instrumentation argument mediaUrl (am instrument -e mediaUrl <url>)")
    }

    @After
    fun release() {
        for (player in players) player.close()
        for (sink in sinks) sink.close()
    }

    @Test
    fun playsThroughSurfaceHandoffsAndStopsDeliveringOnClose() {
        val recorder = Recorder()
        val player = createPlayer(recorder)
        val deliveryThread = threadNamed("mpv-listener-${player.session}")

        val video1 = sink("video-1")
        val osd1 = sink("osd-1")
        player.attachSurfaces(video1.surface, osd1.surface, videoGeneration = 1, osdGeneration = 1)
        player.observeProperty("time-pos", PropertyFormat.Double)
        player.observeProperty("pause", PropertyFormat.Flag)

        val entry = player.command("loadfile", mediaUrl) ?: throw AssertionError("loadfile returned no playlist entry")
        recorder.awaitEvent("playback-restart") { it is MpvEvent.PlaybackRestart && it.sourceId == entry }
        val order = recorder.lifecycle(entry)
        assertEquals(
            "the lifecycle arrived out of mpv's order: ${recorder.calls}",
            listOf("start-file", "hook on_preloaded", "file-loaded", "playback-restart"),
            order,
        )
        recorder.awaitChange("time-pos to advance") {
            it is PropertyChange.Double && it.name == "time-pos" && it.sourceId == entry && it.value > 0.0
        }
        video1.awaitFrames(3, "the first video Surface")

        // A replacement under new generations: the old Surfaces are let go before attachSurfaces returns. Frames
        // the old reader had already been handed may still be counted on its thread, hence the settle time.
        val video2 = sink("video-2")
        val osd2 = sink("osd-2")
        player.attachSurfaces(video2.surface, osd2.surface, videoGeneration = 2, osdGeneration = 2)
        Thread.sleep(SETTLE_MILLIS)
        val leftOnTheOldSurface = video1.frames.get()
        video2.awaitFrames(3, "the replacement video Surface")
        Thread.sleep(SETTLE_MILLIS)
        assertEquals("frames reached a Surface after it was replaced", leftOnTheOldSurface, video1.frames.get())

        // The same Surface object under a new generation (a SurfaceView recreation) is a new Surface; a null
        // OSD retires the OSD plane.
        player.attachSurfaces(video2.surface, null, videoGeneration = 3, osdGeneration = 3)
        video2.awaitNewFrames(5, "the recreated video Surface")

        player.setProperty("pause", true)
        recorder.awaitChange("pause to be reported") { it == PropertyChange.Flag("pause", true, entry) }
        assertEquals(true, player.getFlag("pause"))

        player.close()
        deliveryThread.join(TimeUnit.SECONDS.toMillis(5))
        assertFalse("the delivery thread outlived close", deliveryThread.isAlive)
        val deliveredByClose = recorder.calls.size
        Thread.sleep(500)
        assertEquals("a callback arrived after close", deliveredByClose, recorder.calls.size)
        assertEquals(
            "a callback ran off the delivery thread",
            setOf(deliveryThread.name),
            recorder.threads.toSet(),
        )

        assertNull(player.getString("pause"))
        player.continueHook(1)
        player.close()
        try {
            player.command("stop")
            fail("a closed player ran a command")
        } catch (expected: IllegalStateException) {
        }
    }

    @Test
    fun profile7FelFilterChangesResidualPatchPixelsThroughGpuNext() {
        val withFel = captureResidual(useFel = true)
        val baseOnly = captureResidual(useFel = false)
        try {
            assertEquals(withFel.width, baseOnly.width)
            assertEquals(withFel.height, baseOnly.height)
            val (centerMad, outsideMad, changedPixels) = compareResidualPatch(withFel, baseOnly)
            Log.i(TAG, "P7 synthetic pixel comparison: ${withFel.width}x${withFel.height}, center MAD=$centerMad, outside MAD=$outsideMad, center pixels changed >2=$changedPixels")
            Log.i(TAG, "P7 synthetic samples: FEL=${samplePixels(withFel)}, base=${samplePixels(baseOnly)}")

            assertTrue("FEL residual did not change the center patch (MAD=$centerMad, outside=$outsideMad)", centerMad > 2.0)
            assertTrue("FEL residual change was not localized to the authored patch (center=$centerMad, outside=$outsideMad)", centerMad > outsideMad * 2.0)
            assertTrue("Profile 7 FEL output was not selected", lastFelProfile == "7")
            assertTrue("FEL filter did not select gpu-next", lastFelVo == "gpu-next")
            assertTrue("no center pixels changed by more than two code values", changedPixels > 0)
        } finally {
            withFel.recycle()
            baseOnly.recycle()
        }
    }

    private var lastFelProfile: String? = null
    private var lastFelVo: String? = null

    private fun captureResidual(useFel: Boolean): Bitmap {
        val recorder = Recorder()
        val player = createPlayer(recorder, p7Fel = useFel, gpuNext = true)
        val video = sink(if (useFel) "p7-fel-residual" else "p7-base-residual")
        try {
            player.attachSurfaces(video.surface, null, videoGeneration = 1, osdGeneration = 1)
            player.observeProperty("time-pos", PropertyFormat.Double)
            val entry = player.command("loadfile", mediaUrl)
                ?: throw AssertionError("loadfile returned no playlist entry")
            recorder.awaitEvent("playback-restart") { it is MpvEvent.PlaybackRestart && it.sourceId == entry }
            video.awaitFrames(3, "the ${if (useFel) "FEL" else "base-only"} gpu-next Surface")

            if (useFel) {
                lastFelProfile = player.getString("current-tracks/video/dolby-vision-profile")
                lastFelVo = player.getString("current-vo")
                assertEquals("the synthetic clip was not identified as Profile 7", "7", lastFelProfile)
                assertEquals("the FEL filter did not select gpu-next", "gpu-next", lastFelVo)
                Log.i(TAG, "FEL session properties: vf=${player.getString("vf")}, vo=$lastFelVo, profile=$lastFelProfile, tracks=${player.getString("track-list")}; mpv log=${recorder.diagnosticLog()}")
            }

            player.command("seek", "1.5", "absolute+exact")
            await("seek into the authored residual phase") {
                val position = player.getDouble("time-pos")
                position != null && position >= 1.45
            }
            video.awaitNewFrames(3, "the residual-phase frame")
            player.setProperty("pause", true)
            await("pause at the residual frame") { player.getFlag("pause") == true }
            return copySurfacePixels(video.surface)
        } finally {
            player.close()
            players.remove(player)
            video.close()
            sinks.remove(video)
        }
    }

    private fun copySurfacePixels(surface: android.view.Surface): Bitmap {
        val bitmap = Bitmap.createBitmap(1280, 720, Bitmap.Config.ARGB_8888)
        val thread = HandlerThread("surface-pixel-copy").apply { start() }
        try {
            val finished = CountDownLatch(1)
            var result = Int.MIN_VALUE
            PixelCopy.request(surface, bitmap, { status ->
                result = status
                finished.countDown()
            }, Handler(thread.looper))
            assertTrue("PixelCopy did not finish", finished.await(20, TimeUnit.SECONDS))
            assertEquals("PixelCopy could not read the rendered Surface", PixelCopy.SUCCESS, result)
            return bitmap
        } catch (failure: Throwable) {
            bitmap.recycle()
            throw failure
        } finally {
            thread.quitSafely()
        }
    }

    private fun compareResidualPatch(withFel: Bitmap, baseOnly: Bitmap): Triple<Double, Double, Int> {
        val width = withFel.width
        val height = withFel.height
        // The authored EL residual occupies the middle quarter of the full-resolution picture.
        val x0 = width * 3 / 8
        val x1 = width * 5 / 8
        val y0 = height * 3 / 8
        val y1 = height * 5 / 8
        var centerTotal = 0L
        var centerPixels = 0
        var changedPixels = 0
        for (y in y0 until y1) for (x in x0 until x1) {
            val delta = rgbDelta(withFel.getPixel(x, y), baseOnly.getPixel(x, y))
            centerTotal += delta
            centerPixels++
            if (delta > 2) changedPixels++
        }

        // Compare the same vertical luma stripes above and below the EL patch.
        var outsideTotal = 0L
        var outsidePixels = 0
        val outsideRanges = listOf(0 until height / 4, height * 3 / 4 until height)
        for (range in outsideRanges) for (y in range) for (x in x0 until x1) {
            outsideTotal += rgbDelta(withFel.getPixel(x, y), baseOnly.getPixel(x, y))
            outsidePixels++
        }
        return Triple(centerTotal.toDouble() / centerPixels, outsideTotal.toDouble() / outsidePixels, changedPixels)
    }

    private fun rgbDelta(first: Int, second: Int): Int =
        (kotlin.math.abs(Color.red(first) - Color.red(second)) +
            kotlin.math.abs(Color.green(first) - Color.green(second)) +
            kotlin.math.abs(Color.blue(first) - Color.blue(second))) / 3

    private fun samplePixels(bitmap: Bitmap): String = listOf(0.1, 0.35, 0.65, 0.9)
        .joinToString { fraction ->
            val x = (bitmap.width * fraction).toInt().coerceIn(0, bitmap.width - 1)
            val y = bitmap.height / 2
            "($x,$y)=#%06x".format(bitmap.getPixel(x, y) and 0xffffff)
        }

    private fun await(what: String, satisfied: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        while (!satisfied()) {
            if (System.nanoTime() >= deadline) fail("Timed out waiting for $what")
            Thread.sleep(25)
        }
    }

    @Test
    fun closeFromInsideTheListenerEndsDelivery() {
        lateinit var player: MpvPlayer
        val closedInside = CountDownLatch(1)
        val recorder = object : Recorder() {
            override fun onEvent(event: MpvEvent) {
                super.onEvent(event)
                if (event is MpvEvent.FileLoaded) {
                    player.close()
                    closedInside.countDown()
                }
            }
        }
        player = createPlayer(recorder)
        val deliveryThread = threadNamed("mpv-listener-${player.session}")
        player.attachSurfaces(sink("video").surface, null, videoGeneration = 1, osdGeneration = 1)
        player.command("loadfile", mediaUrl)

        assertTrue("close from inside the listener never returned", closedInside.await(30, TimeUnit.SECONDS))
        deliveryThread.join(TimeUnit.SECONDS.toMillis(5))
        assertFalse("the delivery thread outlived close", deliveryThread.isAlive)
        val deliveredByClose = recorder.calls.size
        Thread.sleep(500)
        assertEquals("a callback arrived after close", deliveredByClose, recorder.calls.size)
        assertTrue(recorder.calls.last().startsWith("event file-loaded"))
    }

    @Test
    fun blockingCallsRefuseTheMainThread() {
        val player = createPlayer(Recorder())
        val video = sink("video")
        val refused = CopyOnWriteArrayList<String>()
        instrumentation.runOnMainSync {
            val calls = mapOf<String, () -> Unit>(
                "create" to { MpvPlayer.create(instrumentation.targetContext, object : MpvPlayer.Listener {}).close() },
                "attachSurfaces" to { player.attachSurfaces(video.surface, null, 1, 1) },
                "observeProperty" to { player.observeProperty("pause", PropertyFormat.Flag) },
                "setLogLevel" to { player.setLogLevel("info") },
                "close" to { player.close() },
            )
            for ((name, call) in calls) {
                if (runCatching(call).exceptionOrNull() is IllegalStateException) refused += name
            }
        }
        assertEquals(listOf("create", "attachSurfaces", "observeProperty", "setLogLevel", "close"), refused.toList())
        // Refused on the main thread, the player is still open off it.
        player.attachSurfaces(video.surface, null, 1, 1)
        assertNotNull(player.getString("idle-active"))
    }

    private fun createPlayer(listener: MpvPlayer.Listener, p7Fel: Boolean = false, gpuNext: Boolean = p7Fel): MpvPlayer =
        MpvPlayer.create(instrumentation.targetContext, listener, hookTimeoutMillis = 10_000) {
            setOption("ao", "null")
            setOption("vo", if (gpuNext) "gpu-next" else "gpu")
            setOption("gpu-context", "android")
            setOption("hwdec", "no")
            setOption("loop-file", "inf")
            if (gpuNext) setOption("vf", "format=enhancement-layer=${if (p7Fel) "yes" else "no"}")
            setLogLevel(if (p7Fel) "debug" else "info")
        }.also { players += it }

    private fun sink(name: String): FrameSink = FrameSink(name).also { sinks += it }

    private fun threadNamed(name: String): Thread = Thread.getAllStackTraces().keys.single { it.name == name }

    /** Records every callback, the thread it ran on, and lets the test wait for one. */
    private open class Recorder : MpvPlayer.Listener {
        val calls = CopyOnWriteArrayList<String>()
        val threads = CopyOnWriteArrayList<String>()
        private val lock = ReentrantLock()
        private val arrived = lock.newCondition()
        private val events = ArrayList<MpvEvent>()
        private val changes = ArrayList<PropertyChange>()
        private val logs = ArrayList<String>()

        override fun onPropertyChange(change: PropertyChange) {
            note("property ${change.name}")
            lock.withLock {
                changes += change
                arrived.signalAll()
            }
        }

        override fun onEvent(event: MpvEvent) {
            note(
                when (event) {
                    is MpvEvent.StartFile -> "event start-file ${event.sourceId}"
                    is MpvEvent.FileLoaded -> "event file-loaded ${event.sourceId}"
                    is MpvEvent.PlaybackRestart -> "event playback-restart ${event.sourceId}"
                    is MpvEvent.EndFile -> "event end-file ${event.sourceId} ${event.reason} ${event.errorMessage}"
                    is MpvEvent.LogMessage -> "log"
                },
            )
            lock.withLock {
                if (event is MpvEvent.LogMessage) logs += "[${event.prefix}] ${event.text}" else events += event
                arrived.signalAll()
            }
        }

        override fun onHook(player: MpvPlayer, name: String, id: Long) {
            note("hook $name")
            super.onHook(player, name, id)
        }

        /** The lifecycle callbacks of playlist entry [entry] and the hooks among them, in delivery order. */
        fun lifecycle(entry: Long): List<String> {
            val start = calls.indexOf("event start-file $entry")
            return calls.drop(start.coerceAtLeast(0))
                .filter { it.startsWith("hook ") || (it.startsWith("event ") && it.endsWith(" $entry")) }
                .map { it.removePrefix("event ").removeSuffix(" $entry") }
                .take(4)
        }

        fun awaitEvent(what: String, matches: (MpvEvent) -> Boolean) = await(what) { events.any(matches) }

        fun awaitChange(what: String, matches: (PropertyChange) -> Boolean) = await(what) { changes.any(matches) }

        fun diagnosticLog(): String = lock.withLock {
            logs.filter {
                val line = it.lowercase()
                "dolby" in line || "dovi" in line || "enhancement" in line || "vf/" in line ||
                    "ffmpeg" in line || "[vd]" in line || "failed" in line || "decoder" in line
            }.takeLast(50).joinToString(" | ") { it.take(320) }
        }

        private fun await(what: String, satisfied: () -> Boolean) {
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
            lock.withLock {
                while (!satisfied()) {
                    val failed = events.filterIsInstance<MpvEvent.EndFile>().firstOrNull { it.reason == EndFileReason.Error }
                    if (failed != null) fail("Playback failed while waiting for $what: $failed; log: ${logs.takeLast(20)}")
                    val leftNanos = deadline - System.nanoTime()
                    if (leftNanos <= 0) fail("Timed out waiting for $what; callbacks: ${calls.takeLast(30)}; log: ${logs.takeLast(20)}")
                    arrived.awaitNanos(leftNanos)
                }
            }
        }

        private fun note(call: String) {
            calls += call
            threads += Thread.currentThread().name
        }
    }

    /** An ImageReader Surface that consumes and counts every frame queued to it, on its own thread. */
    private class FrameSink(name: String) : AutoCloseable {
        private val thread = HandlerThread("sink-$name").apply { start() }
        private val reader = ImageReader.newInstance(1280, 720, ImageFormat.PRIVATE, 3)
        private val arrivals = Semaphore(0)
        val frames = AtomicInteger()
        val surface = reader.surface

        init {
            reader.setOnImageAvailableListener({
                it.acquireLatestImage()?.close()
                frames.incrementAndGet()
                arrivals.release()
            }, Handler(thread.looper))
        }

        fun awaitFrames(count: Int, what: String) {
            assertTrue(
                "only ${frames.get()} frames reached $what",
                arrivals.tryAcquire(count, 20, TimeUnit.SECONDS),
            )
        }

        /** Like [awaitFrames], counting only frames that arrive from now on. */
        fun awaitNewFrames(count: Int, what: String) {
            arrivals.drainPermits()
            awaitFrames(count, what)
        }

        override fun close() {
            reader.close()
            thread.quitSafely()
        }
    }

    private companion object {
        const val TAG = "MpvPlayerDeviceTest"
        /** Long enough for frames already handed to a reader to be counted on its thread. */
        const val SETTLE_MILLIS = 300L
    }
}
