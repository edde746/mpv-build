package com.edde746.mpv

import android.graphics.Color
import android.os.Handler
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/** [MpvPlaceholderSurface] against the device's EGL and BufferQueue; no media or window needed. */
@RunWith(AndroidJUnit4::class)
class MpvPlaceholderSurfaceDeviceTest {
    @Test
    fun aFrameQueuedBehindCloseIsDroppedOnTheStoppingThread() {
        val placeholder = MpvPlaceholderSurface.create()
        // The GL thread is the placeholder's own; the test holds it so the teardown and a frame queue in this order.
        val handler = MpvPlaceholderSurface::class.java.getDeclaredField("handler").run {
            isAccessible = true
            get(placeholder) as Handler
        }
        val glThread = handler.looper.thread
        val uncaught = AtomicReference<Throwable?>()
        glThread.setUncaughtExceptionHandler { _, error -> uncaught.set(error) }
        val hold = CountDownLatch(1)
        handler.post { hold.await() }

        placeholder.close()
        // A producer still finishing a frame when the consumer goes: its frame-available callback queues behind
        // the teardown, which quitSafely still delivers.
        val canvas = placeholder.surface.lockCanvas(null)
        canvas.drawColor(Color.BLACK)
        placeholder.surface.unlockCanvasAndPost(canvas)
        hold.countDown()

        glThread.join(5_000)
        assertNull("the late frame threw on the GL thread", uncaught.get())
        assertFalse("the GL thread outlived close", glThread.isAlive)
    }

    @Test
    fun framesPostedBeforeTheConsumerIsUpAreTaken() {
        // create returns with the Surface while its GL thread still builds the context (API 26+): a producer that
        // starts at once posts into a queue nothing latches yet, and must get every buffer back once it is up.
        val placeholder = MpvPlaceholderSurface.create()
        val frames = 12
        val posted = CountDownLatch(frames)
        val producer = thread(name = "placeholder-producer") {
            repeat(frames) {
                val canvas = placeholder.surface.lockCanvas(null)
                canvas.drawColor(Color.BLACK)
                placeholder.surface.unlockCanvasAndPost(canvas)
                posted.countDown()
            }
        }
        try {
            assertTrue(
                "the producer stalled after ${frames - posted.count} frames: nothing took them",
                posted.await(10, TimeUnit.SECONDS),
            )
        } finally {
            producer.join(1_000)
            placeholder.close()
        }
    }
}
