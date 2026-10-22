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
import android.view.Surface
import java.util.concurrent.Callable
import java.util.concurrent.FutureTask

/**
 * The Surface mpv holds while no real video Surface exists (before the first `surfaceCreated`, and between a
 * `surfaceDestroyed` and the next one).
 *
 * An offscreen consumer, not an undrained ImageReader queue: MediaCodec and GPU output can both keep posting buffers
 * while the real SurfaceViews are absent, and a consumer that stops acquiring stalls the producer. Each frame is
 * latched and dropped on this object's own GL thread, including while the main thread waits for mpv to acknowledge a
 * surface handoff. It is the consumer Media3's `EGLSurfaceTexture` makes, without the Media3 dependency.
 */
class MpvPlaceholderSurface private constructor(
    /** The Surface to give mpv; valid until [close]. */
    val surface: Surface,
    private val handler: Handler,
    private val gl: Gl,
) : AutoCloseable {

    private class Gl(
        val display: EGLDisplay,
        val context: EGLContext,
        val pbuffer: EGLSurface,
        val texture: Int,
        val surfaceTexture: SurfaceTexture,
    )

    companion object {
        /**
         * Starts the GL thread and blocks until its consumer is up; call it off the main thread. Throws
         * [IllegalStateException] when EGL refuses a step, with the thread already stopped.
         */
        fun create(): MpvPlaceholderSurface {
            val thread = HandlerThread("MpvPlaceholder").apply { start() }
            val handler = Handler(thread.looper)
            val task = FutureTask(Callable { initGl(handler) })
            handler.post(task)
            val gl = try {
                task.get()
            } catch (e: Exception) {
                thread.quitSafely()
                throw IllegalStateException("Placeholder surface initialization failed", e.cause ?: e)
            }
            return MpvPlaceholderSurface(Surface(gl.surfaceTexture), handler, gl)
        }

        private fun initGl(handler: Handler): Gl {
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
                val surfaceTexture = SurfaceTexture(textures[0])
                // Latch and drop every frame so the producer never runs out of buffers.
                surfaceTexture.setOnFrameAvailableListener({ it.updateTexImage() }, handler)
                return Gl(display, context, pbuffer, textures[0], surfaceTexture)
            } catch (e: Throwable) {
                releaseEgl(display, context, pbuffer)
                throw e
            }
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
                surface.release()
                gl.surfaceTexture.release()
                GLES20.glDeleteTextures(1, intArrayOf(gl.texture), 0)
                releaseEgl(gl.display, gl.context, gl.pbuffer)
            } finally {
                handler.looper.quitSafely()
            }
        }
    }
}
