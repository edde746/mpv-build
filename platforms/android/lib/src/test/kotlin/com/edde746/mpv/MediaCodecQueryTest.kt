// Derived from Plezy's MediaCodecQueryTest.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.MediaCodecInfoBuilder

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25, 28])
class MediaCodecQueryTest {
    @Test
    fun aQueryBeforeApi29NeverCallsTheApi29HardwareFlag() {
        val codecInfo = MediaCodecInfoBuilder.newBuilder()
            .setName("OMX.qcom.video.decoder.avc")
            .setIsHardwareAccelerated(false)
            .build()

        assertTrue(MediaCodecQuery.isHardwareAccelerated(codecInfo))
    }

    @Test
    fun recognizesKnownPlatformAndFfmpegSoftwareCodecNames() {
        listOf(
            "OMX.google.h264.decoder",
            "OMX.FFMPEG.VIDEO.DECODER",
            "c2.android.avc.decoder",
            "c2.android.av1-dav1d.decoder",
            "c2.ffmpeg.vp9.decoder",
            "vendor.video.sw.decoder",
        ).forEach { name ->
            assertTrue("expected software codec: $name", MediaCodecQuery.isSoftwareCodecName(name))
        }
    }

    @Test
    fun doesNotMisclassifyVendorHardwareCodecNames() {
        listOf(
            "OMX.qcom.video.decoder.avc",
            "OMX.MTK.VIDEO.DECODER.HEVC",
            "c2.qti.avc.decoder",
            "c2.exynos.hevc.decoder",
            "c2.google.av1.decoder",
        ).forEach { name ->
            assertFalse("expected hardware codec: $name", MediaCodecQuery.isSoftwareCodecName(name))
        }
    }

    @Test
    fun fromApi29ThePlatformFlagAndANonSoftwareNameMustAgree() {
        assertFalse(MediaCodecQuery.isHardwareAccelerated(29, true, "c2.ffmpeg.aac.decoder"))
        assertFalse(MediaCodecQuery.isHardwareAccelerated(29, false, "c2.qti.avc.decoder"))
        assertTrue(MediaCodecQuery.isHardwareAccelerated(29, true, "c2.qti.avc.decoder"))
        // Tensor's BigOcean AV1 block (plezy#2272): the platform flag decides.
        assertTrue(MediaCodecQuery.isHardwareAccelerated(33, true, "c2.google.av1.decoder"))
    }

    @Test
    fun beforeApi29TheNameDecides() {
        assertFalse(MediaCodecQuery.isHardwareAccelerated(28, true, "OMX.google.h264.decoder"))
        assertTrue(MediaCodecQuery.isHardwareAccelerated(28, false, "OMX.qcom.video.decoder.avc"))
    }

    @Test
    fun mapsGatedCodecsToTheMimeTypesDecodersAdvertise() {
        assertEquals(
            mapOf("hevc" to true, "av1" to true),
            MediaCodecQuery.hardwareVideoDecodeSupport(setOf("video/avc", "video/hevc", "video/av01", "audio/mp4a-latm")),
        )
        assertEquals(
            mapOf("hevc" to true, "av1" to false),
            MediaCodecQuery.hardwareVideoDecodeSupport(setOf("video/avc", "video/hevc")),
        )
    }
}
