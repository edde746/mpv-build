// Derived from Plezy's MpvPlaceholderSurface.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask

/**
 * The Surface mpv holds while no real video Surface exists (before the first `surfaceCreated`, and between a
 * `surfaceDestroyed` and the next one).
 *
 * An offscreen consumer, not an undrained ImageReader queue: MediaCodec and GPU output can both keep posting buffers
 * while the real SurfaceViews are absent, and a consumer that stops acquiring stalls the producer. Each frame is
 * latched and dropped on this object's own GL thread, including while the main thread waits for mpv to acknowledge a
 * surface handoff. It is the consumer Media3's `EGLSurfaceTexture` makes, without the Media3 dependency, and like it
 * never throws on that thread, where an uncaught exception ends the process: a frame that fails to latch, or whose
 * callback was already queued when [close] tore the consumer down, is dropped.
 */
class MpvPlaceholderSurface private constructor(
    private val handler: Handler,
    private val consumer: Consumer,
) : AutoCloseable {

    /** The Surface to give mpv; valid until [close]. */
    val surface: Surface get() = consumer.surface

    /**
     * The queue's two ends, and what its GL thread latches with. [egl], [released] and [latchFailureLogged] are read
     * and written on that thread only; [egl] is null until the thread has built it.
     */
    private class Consumer(val surfaceTexture: SurfaceTexture, val surface: Surface) {
        var egl: Egl? = null
        var released = false
        var latchFailureLogged = false
    }

    private class Egl(val display: EGLDisplay, val context: EGLContext, val pbuffer: EGLSurface, val texture: Int)

    companion object {
        private const val TAG = "MpvPlaceholderSurface"

        /**
         * Makes the Surface and starts the GL thread that takes its frames; call it off the main thread.
         *
         * From API 26 this returns as soon as the Surface exists, and the thread builds its EGL context behind it:
         * that build is the slow part (10 ms on a Pixel 7, 17-28 ms on MT8696 and Amlogic TV boxes, 70-90 ms on a
         * SHIELD TV), and a player is usually created right after, which need not wait for it. The SurfaceTexture
         * starts detached, as a TextureView's does, and frames posted meanwhile are taken once the context is up. An
         * EGL that refuses is logged and the Surface released, so a producer pointed at it fails instead of stalling.
         *
         * On API 25, where a SurfaceTexture cannot start detached, this blocks until the consumer is up and throws
         * [IllegalStateException] when EGL refuses a step or the wait is interrupted (the interrupt status is kept).
         * Either way the thread stops and nothing it built is left behind.
         */
        fun create(): MpvPlaceholderSurface {
            val thread = HandlerThread("MpvPlaceholder").apply { start() }
            val handler = Handler(thread.looper)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                val surfaceTexture = SurfaceTexture(false)
                val consumer = Consumer(surfaceTexture, Surface(surfaceTexture))
                // Posted first, so every frame callback finds the context built or the consumer released.
                handler.post { attach(consumer, thread) }
                surfaceTexture.setOnFrameAvailableListener({ latchAndDrop(consumer) }, handler)
                return MpvPlaceholderSurface(handler, consumer)
            }
            val task = FutureTask(
                Callable {
                    val egl = initEgl()
                    try {
                        val surfaceTexture = SurfaceTexture(egl.texture)
                        val consumer = Consumer(surfaceTexture, Surface(surfaceTexture)).also { it.egl = egl }
                        surfaceTexture.setOnFrameAvailableListener({ latchAndDrop(consumer) }, handler)
                        consumer
                    } catch (e: Throwable) {
                        releaseEgl(egl)
                        throw e
                    }
                },
            )
            handler.post(task)
            val consumer = try {
                task.get()
            } catch (e: ExecutionException) {
                // The task released what it had built before throwing.
                thread.quitSafely()
                throw IllegalStateException("Placeholder surface initialization failed", e.cause ?: e)
            } catch (e: InterruptedException) {
                // The task may still finish. It runs first on its thread, so this teardown finds it done.
                handler.post {
                    try {
                        runCatching { task.get() }.getOrNull()?.let(::release)
                    } finally {
                        thread.quitSafely()
                    }
                }
                Thread.currentThread().interrupt()
                throw IllegalStateException("Placeholder surface initialization interrupted", e)
            }
            return MpvPlaceholderSurface(handler, consumer)
        }

        /** Builds the context and attaches the detached SurfaceTexture to it; on the GL thread, before any frame. */
        private fun attach(consumer: Consumer, thread: HandlerThread) {
            val egl = try {
                initEgl()
            } catch (e: Throwable) {
                fail(consumer, thread, e)
                return
            }
            try {
                consumer.surfaceTexture.attachToGLContext(egl.texture)
                consumer.egl = egl
            } catch (e: Throwable) {
                releaseEgl(egl)
                fail(consumer, thread, e)
            }
        }

        /**
         * No consumer can be built: nothing would ever take a frame, so the Surface goes too. Whoever holds it gets
         * an invalid Surface (mpv then refuses the handoff or fails the output), not a producer blocked for good.
         */
        private fun fail(consumer: Consumer, thread: HandlerThread, error: Throwable) {
            Log.e(TAG, "Placeholder surface initialization failed; releasing its Surface", error)
            consumer.released = true
            consumer.surface.release()
            consumer.surfaceTexture.release()
            thread.quitSafely()
        }

        /** A current ES2 context on a 1x1 pbuffer and one texture name; throws after undoing what it had built. */
        private fun initEgl(): Egl {
            val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            check(display != EGL14.EGL_NO_DISPLAY) { "eglGetDisplay failed" }
            val version = IntArray(2)
            check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize failed: ${EGL14.eglGetError()}" }
            var context = EGL14.EGL_NO_CONTEXT
            var pbuffer = EGL14.EGL_NO_SURFACE
            try {
                val configs = arrayOfNulls<EGLConfig>(1)
                val count = IntArray(1)
                val attributes = intArrayOf(
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                    EGL14.EGL_NONE,
                )
                check(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0) {
                    "eglChooseConfig failed: ${EGL14.eglGetError()}"
                }
                val config = configs[0]
                context = EGL14.eglCreateContext(
                    display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
                )
                check(context != EGL14.EGL_NO_CONTEXT) { "eglCreateContext failed: ${EGL14.eglGetError()}" }
                pbuffer = EGL14.eglCreatePbufferSurface(display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
                check(pbuffer != EGL14.EGL_NO_SURFACE) { "eglCreatePbufferSurface failed: ${EGL14.eglGetError()}" }
                check(EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)) { "eglMakeCurrent failed: ${EGL14.eglGetError()}" }
                val textures = IntArray(1)
                GLES20.glGenTextures(1, textures, 0)
                return Egl(display, context, pbuffer, textures[0])
            } catch (e: Throwable) {
                releaseEgl(display, context, pbuffer)
                throw e
            }
        }

        /** Latches the newest frame so the producer gets its buffer back; nothing ever reads it. */
        private fun latchAndDrop(consumer: Consumer) {
            if (consumer.released) return
            try {
                consumer.surfaceTexture.updateTexImage()
            } catch (e: RuntimeException) {
                // A frame the driver cannot latch is dropped like the rest; the next one is latched afresh.
                if (!consumer.latchFailureLogged) {
                    consumer.latchFailureLogged = true
                    Log.w(TAG, "Dropped a frame that failed to latch", e)
                }
            }
        }

        /** Tears down the consumer, once; on the GL thread. */
        private fun release(consumer: Consumer) {
            if (consumer.released) return
            consumer.released = true
            consumer.surface.release()
            consumer.surfaceTexture.release()
            consumer.egl?.let(::releaseEgl)
        }

        private fun releaseEgl(egl: Egl) {
            GLES20.glDeleteTextures(1, intArrayOf(egl.texture), 0)
            releaseEgl(egl.display, egl.context, egl.pbuffer)
        }

        private fun releaseEgl(display: EGLDisplay, context: EGLContext, pbuffer: EGLSurface) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (pbuffer != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, pbuffer)
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
            EGL14.eglReleaseThread()
            // Android's EGLDisplay is reference-counted: every eglInitialize needs its eglTerminate.
            EGL14.eglTerminate(display)
        }
    }

    /** Call only after native consumers have retired; destruction stays on the GL thread, which then stops. */
    override fun close() {
        handler.post {
            try {
                release(consumer)
            } finally {
                handler.looper.quitSafely()
            }
        }
    }
}
