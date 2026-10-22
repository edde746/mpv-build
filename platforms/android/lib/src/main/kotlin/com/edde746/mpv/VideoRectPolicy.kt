// Derived from Plezy's VideoRectPolicy.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/**
 * Pure geometry for the video plane's surface rectangle.
 *
 * `vo=mediacodec` scales decoded buffers to the whole Surface and implements none of mpv's src/dst rect math, so
 * aspect, cover and zoom are view geometry: the video surface is sized to the rectangle the image should occupy and
 * the container clips whatever falls outside it, while the subtitle (OSD) view keeps the container's full size. A
 * stretch needs no term of its own: with mpv's `video-aspect-override` the display size (`dwidth`×`dheight`) already
 * has the container's shape, but for mpv's whole pixels (the override goes to 4 decimals, and mpv drops the fraction
 * of the side it scales); a picture within a pixel of the container's shape takes the container's size exactly, so a
 * stretch leaves no 1 px bar.
 */
object VideoRectPolicy {
    /**
     * `video-zoom` bounds, well outside a player's own zoom control: the value is reachable from a user's mpv.conf,
     * where an absurd one would be a compositor allocation.
     */
    private const val MAX_ZOOM_LOG2 = 2f

    data class Size(val width: Int, val height: Int)

    /**
     * Laid-out video bounds in the full-size OSD view's coordinate space, mpv's `vo-mediacodec-video-rect`; signed,
     * since a cropped or zoomed picture runs past the view's edges.
     */
    data class Rect(val viewportWidth: Int, val viewportHeight: Int, val left: Int, val top: Int, val right: Int, val bottom: Int) {
        /** The `vo-mediacodec-video-rect` property value. */
        fun propertyValue(): String = "$viewportWidth,$viewportHeight,$left,$top,$right,$bottom"
    }

    /**
     * The picture's bounds as laid out, not clipped to the viewport (cover and zoom need the signed margins), or null
     * while the layout has no viewport or an empty picture.
     */
    fun rectFor(viewportWidth: Int, viewportHeight: Int, left: Int, top: Int, right: Int, bottom: Int): Rect? {
        if (viewportWidth <= 0 || viewportHeight <= 0 || right <= left || bottom <= top) return null
        return Rect(viewportWidth, viewportHeight, left, top, right, bottom)
    }

    /**
     * Size for the video surface, or null when a dimension is not known yet. [panscan] is mpv's 0..1 property: 0 fits
     * the picture inside the container (letterbox), 1 fills it (cropped), values between interpolate the scale.
     * [videoZoomLog2] is mpv's `video-zoom`, a power of two, applied on top and bounded to ±2.
     */
    fun sizeFor(
        containerWidth: Int,
        containerHeight: Int,
        videoWidth: Int,
        videoHeight: Int,
        panscan: Float = 0f,
        videoZoomLog2: Float = 0f,
    ): Size? {
        if (containerWidth <= 0 || containerHeight <= 0 || videoWidth <= 0 || videoHeight <= 0) return null
        val zoom = 2f.pow(videoZoomLog2.coerceIn(-MAX_ZOOM_LOG2, MAX_ZOOM_LOG2))
        // Its width at the container's shape less than a pixel off (|w·H − W·h| < H), or its height (< W).
        if (abs(videoWidth.toLong() * containerHeight - containerWidth.toLong() * videoHeight) < max(containerWidth, containerHeight)) {
            return Size((containerWidth * zoom).toInt().coerceAtLeast(1), (containerHeight * zoom).toInt().coerceAtLeast(1))
        }
        val fit = min(containerWidth.toFloat() / videoWidth, containerHeight.toFloat() / videoHeight)
        val cover = max(containerWidth.toFloat() / videoWidth, containerHeight.toFloat() / videoHeight)
        val scale = (fit + (cover - fit) * panscan.coerceIn(0f, 1f)) * zoom
        return Size(
            width = (videoWidth * scale).toInt().coerceAtLeast(1),
            height = (videoHeight * scale).toInt().coerceAtLeast(1),
        )
    }
}
