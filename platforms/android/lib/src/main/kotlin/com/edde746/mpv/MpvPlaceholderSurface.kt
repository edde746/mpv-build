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
    private val gl: Gl,
) : AutoCloseable {

    /** The Surface to give mpv; valid until [close]. */
    val surface: Surface get() = gl.surface

    /** What the GL thread built; [released] and [latchFailureLogged] are read and written on that thread only. */
    private class Gl(
        val display: EGLDisplay,
        val context: EGLContext,
        val pbuffer: EGLSurface,
        val texture: Int,
        val surfaceTexture: SurfaceTexture,
        val surface: Surface,
    ) {
        var released = false
        var latchFailureLogged = false
    }

    companion object {
        private const val TAG = "MpvPlaceholderSurface"

        /**
         * Starts the GL thread and blocks until its consumer is up; call it off the main thread. Throws
         * [IllegalStateException] when EGL refuses a step or the wait is interrupted (the interrupt status is kept);
         * either way the thread stops and nothing it built is left behind.
         */
        fun create(): MpvPlaceholderSurface {
            val thread = HandlerThread("MpvPlaceholder").apply { start() }
            val handler = Handler(thread.looper)
            val task = FutureTask(Callable { initGl(handler) })
            handler.post(task)
            val gl = try {
                task.get()
            } catch (e: ExecutionException) {
                // initGl released what it had built before throwing.
                thread.quitSafely()
                throw IllegalStateException("Placeholder surface initialization failed", e.cause ?: e)
            } catch (e: InterruptedException) {
                // initGl may still finish. It runs first on its thread, so this teardown finds it done.
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
            return MpvPlaceholderSurface(handler, gl)
        }

        private fun initGl(handler: Handler): Gl {
            val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            check(display != EGL14.EGL_NO_DISPLAY) { "eglGetDisplay failed" }
            val version = IntArray(2)
            check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize failed: ${EGL14.eglGetError()}" }
            var context = EGL14.EGL_NO_CONTEXT
            var pbuffer = EGL14.EGL_NO_SURFACE
            var texture = 0
            var surfaceTexture: SurfaceTexture? = null
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
                texture = textures[0]
                surfaceTexture = SurfaceTexture(texture)
                val gl = Gl(display, context, pbuffer, texture, surfaceTexture, Surface(surfaceTexture))
                surfaceTexture.setOnFrameAvailableListener({ latchAndDrop(gl) }, handler)
                return gl
            } catch (e: Throwable) {
                surfaceTexture?.release()
                if (texture != 0) GLES20.glDeleteTextures(1, intArrayOf(texture), 0)
                releaseEgl(display, context, pbuffer)
                throw e
            }
        }

        /** Latches the newest frame so the producer gets its buffer back; nothing ever reads it. */
        private fun latchAndDrop(gl: Gl) {
            if (gl.released) return
            try {
                gl.surfaceTexture.updateTexImage()
            } catch (e: RuntimeException) {
                // A frame the driver cannot latch is dropped like the rest; the next one is latched afresh.
                if (!gl.latchFailureLogged) {
                    gl.latchFailureLogged = true
                    Log.w(TAG, "Dropped a frame that failed to latch", e)
                }
            }
        }

        /** Tears down what [initGl] built, once; on the GL thread. */
        private fun release(gl: Gl) {
            if (gl.released) return
            gl.released = true
            gl.surface.release()
            gl.surfaceTexture.release()
            GLES20.glDeleteTextures(1, intArrayOf(gl.texture), 0)
            releaseEgl(gl.display, gl.context, gl.pbuffer)
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
                release(gl)
            } finally {
                handler.looper.quitSafely()
            }
        }
    }
}
