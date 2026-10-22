// Derived from Plezy's VideoRectPolicyTest.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VideoRectPolicyTest {
    /** A 16:9 picture on a 20:9 phone in landscape (2400 × 1080). */
    private fun size(panscan: Float = 0f, zoomLog2: Float = 0f) = VideoRectPolicy.sizeFor(2400, 1080, 1920, 1080, panscan, zoomLog2)

    @Test
    fun fitsWithBarsAtNoPanscan() {
        assertEquals(VideoRectPolicy.Size(1920, 1080), size())
        // 2.39:1 on 16:9: the full width, bars top and bottom.
        assertEquals(VideoRectPolicy.Size(1920, 803), VideoRectPolicy.sizeFor(1920, 1080, 2390, 1000))
        // Tall content: the full height, bars left and right.
        val tall = VideoRectPolicy.sizeFor(1920, 1080, 1000, 2390)!!
        assertEquals(1080, tall.height)
        assertTrue(tall.width < 1920)
    }

    @Test
    fun coversTheContainerAtFullPanscan() {
        // As wide as the container; the height runs past it and the container crops it. The regression this exists
        // for: cover was a no-op on the plane.
        assertEquals(VideoRectPolicy.Size(2400, 1350), size(panscan = 1f))
        // Scope content in a portrait container: cover over fit exceeds 4, which a fit-relative clamp would truncate
        // back into letterboxing.
        assertTrue(VideoRectPolicy.sizeFor(1080, 2400, 2390, 1000, panscan = 1f)!!.height >= 2400)
    }

    @Test
    fun panscanInterpolatesBetweenFitAndCover() {
        val fit = VideoRectPolicy.sizeFor(1920, 1080, 2390, 1000)!!
        val half = VideoRectPolicy.sizeFor(1920, 1080, 2390, 1000, panscan = 0.5f)!!
        val cover = VideoRectPolicy.sizeFor(1920, 1080, 2390, 1000, panscan = 1f)!!
        assertTrue(half.height > fit.height)
        assertTrue(half.height < cover.height)
    }

    @Test
    fun zoomScalesOnTopOfTheFit() {
        assertEquals(VideoRectPolicy.Size(960, 540), size(zoomLog2 = -1f))
        assertEquals(VideoRectPolicy.Size(3840, 2160), size(zoomLog2 = 1f))
        assertEquals(VideoRectPolicy.Size(4800, 2700), size(panscan = 1f, zoomLog2 = 1f))
    }

    @Test
    fun anAbsurdZoomIsBoundedRatherThanAllocatingAnUnboundedSurface() {
        assertEquals(VideoRectPolicy.Size(7680, 4320), size(zoomLog2 = 12f))
        assertEquals(VideoRectPolicy.Size(480, 270), size(zoomLog2 = -12f))
    }

    @Test
    fun aStretchedPictureFillsTheContainer() {
        // mpv's video-aspect-override gives the display size the container's shape: the fit then leaves no bars.
        assertEquals(VideoRectPolicy.Size(2400, 1080), VideoRectPolicy.sizeFor(2400, 1080, 2400, 1080))
    }

    @Test
    fun aStretchAPixelShortOfTheContainerFillsIt() {
        // 2400 × 1080 at the override's 2.2222: mpv's display size is 2399 × 1080.
        assertEquals(VideoRectPolicy.Size(2400, 1080), VideoRectPolicy.sizeFor(2400, 1080, 2399, 1080))
        assertEquals(VideoRectPolicy.Size(4800, 2160), VideoRectPolicy.sizeFor(2400, 1080, 2399, 1080, videoZoomLog2 = 1f))
        // Upright, 1080 × 2400 at 0.45: a 1920 × 1080 picture's display size is 1920 × 4266.
        assertEquals(VideoRectPolicy.Size(1080, 2400), VideoRectPolicy.sizeFor(1080, 2400, 1920, 4266))
        // More than a pixel off the container's shape either way (2397 wide, or 1078.65 high, at its shape): its own.
        assertEquals(VideoRectPolicy.Size(2397, 1080), VideoRectPolicy.sizeFor(2400, 1080, 2397, 1080))
    }

    @Test
    fun unknownSizesGiveNothing() {
        assertNull(VideoRectPolicy.sizeFor(0, 1080, 1920, 1080))
        assertNull(VideoRectPolicy.sizeFor(1920, 0, 1920, 1080))
        assertNull(VideoRectPolicy.sizeFor(2400, 1080, 0, 0))
    }

    @Test
    fun theRectKeepsSignedMarginsPastTheViewport() {
        assertEquals("2400,1080,0,-135,2400,1215", VideoRectPolicy.rectFor(2400, 1080, 0, -135, 2400, 1215)?.propertyValue())
        assertEquals("1920,1080,-960,-540,2880,1620", VideoRectPolicy.rectFor(1920, 1080, -960, -540, 2880, 1620)?.propertyValue())
    }

    @Test
    fun noLayoutOrAnEmptyPictureHasNoRect() {
        assertNull(VideoRectPolicy.rectFor(0, 1080, 0, 0, 1920, 1080))
        assertNull(VideoRectPolicy.rectFor(2400, 1080, 10, 0, 10, 1080))
        assertNull(VideoRectPolicy.rectFor(1920, 1080, 0, 80, 1920, 70))
    }
}
