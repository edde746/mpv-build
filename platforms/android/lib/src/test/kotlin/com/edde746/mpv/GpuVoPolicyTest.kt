// Derived from Plezy's GpuVoPolicyTest.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import android.view.Display
import com.edde746.mpv.GpuVoPolicy.DECODE_STALL_MIN_CACHE_SECONDS
import com.edde746.mpv.GpuVoPolicy.DECODE_STALL_SECONDS
import com.edde746.mpv.GpuVoPolicy.DecodeSample
import com.edde746.mpv.GpuVoPolicy.DecodeStall
import com.edde746.mpv.GpuVoPolicy.DvDecoderCandidate
import com.edde746.mpv.GpuVoPolicy.DvDecoderOptions
import com.edde746.mpv.GpuVoPolicy.DvRoute
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * P5 has no compatible base layer: on a device without a converting decoder it must leave the video plane for gpu-next
 * software reshaping, and nothing else may be routed there (wrong colours verified on a Pixel 7). The vo target matrix
 * keeps gpu-next away from hardware sessions (plezy#2010) while giving the reshaping path the only vo that composites
 * RPU metadata (plezy#1902).
 */
class GpuVoPolicyTest {
    @Test
    fun profile5WithoutAConvertingDecoderIsReshapedOnGpuNext() {
        assertTrue(GpuVoPolicy.needsDvReshaping(5, "auto", nativeP5Decoder = false, softwareDecode = false))
        assertFalse(GpuVoPolicy.needsDvReshaping(5, "auto", nativeP5Decoder = true, softwareDecode = false))
        assertTrue(
            "hwdec=no reaches no converting decoder",
            GpuVoPolicy.needsDvReshaping(5, "auto", nativeP5Decoder = true, softwareDecode = true),
        )
    }

    @Test
    fun baseLayerCompatibleProfilesAreNeverReshaped() {
        // P7 and P8 strip to an HDR10 or HLG base layer; non-DV content has no profile.
        for (profile in listOf(7, 8, null)) {
            for (softwareDecode in listOf(false, true)) {
                assertFalse(
                    "P$profile software=$softwareDecode",
                    GpuVoPolicy.needsDvReshaping(profile, "auto", nativeP5Decoder = false, softwareDecode = softwareDecode),
                )
            }
        }
    }

    @Test
    fun explicitConversionModesAreUserOverridesAndStayNative() {
        for (mode in listOf("disabled", "native", "dv81", "hevc", "hevc_strip")) {
            assertFalse(mode, GpuVoPolicy.needsDvReshaping(5, mode, nativeP5Decoder = false, softwareDecode = false))
            assertFalse(mode, GpuVoPolicy.needsDvReshaping(5, mode, nativeP5Decoder = false, softwareDecode = true))
        }
    }

    @Test
    fun aDvDisplayTakesEveryProfileOnTheDvDecoder() {
        for (profile in listOf(null, 5, 7, 8)) {
            for (p5Decoder in listOf(false, true)) {
                assertEquals(
                    "profile=$profile p5=$p5Decoder",
                    DvDecoderOptions(dolbyVision = true, p7Mode = "auto"),
                    GpuVoPolicy.dvDecoderOptions("auto", displaySupportsDv = true, doviProfile = profile, nativeP5Decoder = p5Decoder),
                )
            }
        }
    }

    @Test
    fun withoutADvDisplayOnlyP5WithAConvertingDecoderStaysOnTheDvPath() {
        // P5 has no compatible base layer: the decoder that advertises it is the only hardware path (plezy#2290).
        assertEquals(
            DvDecoderOptions(dolbyVision = true, p7Mode = "strip"),
            GpuVoPolicy.dvDecoderOptions("auto", displaySupportsDv = false, doviProfile = 5, nativeP5Decoder = true),
        )
        // Nothing on the device converts P5, so software reshaping has to.
        assertEquals(
            DvDecoderOptions(dolbyVision = false, p7Mode = "strip"),
            GpuVoPolicy.dvDecoderOptions("auto", displaySupportsDv = false, doviProfile = 5, nativeP5Decoder = false),
        )
        // P8 decodes its compatible base layer as plain HEVC whatever the device ships: a DV decoder without a DV sink
        // may convert to SDR (plezy#2416). P7 and non-DV content: the DV decoder has nothing to take.
        for (p5Decoder in listOf(false, true)) {
            for (profile in listOf(8, 7, null)) {
                assertEquals(
                    "profile=$profile p5=$p5Decoder",
                    DvDecoderOptions(dolbyVision = false, p7Mode = "strip"),
                    GpuVoPolicy.dvDecoderOptions("auto", displaySupportsDv = false, doviProfile = profile, nativeP5Decoder = p5Decoder),
                )
            }
        }
    }

    @Test
    fun explicitConversionModesIgnoreTheDeviceAndFileAndAnUnknownModeIsRejected() {
        for (display in listOf(false, true)) {
            for (profile in listOf(null, 5, 8)) {
                assertEquals(
                    "disabled/$display/$profile",
                    DvDecoderOptions(dolbyVision = true, p7Mode = "native"),
                    GpuVoPolicy.dvDecoderOptions("disabled", display, profile, nativeP5Decoder = false),
                )
                assertEquals(
                    "dv81/$display/$profile",
                    DvDecoderOptions(dolbyVision = true, p7Mode = "convert"),
                    GpuVoPolicy.dvDecoderOptions("dv81", display, profile, nativeP5Decoder = false),
                )
                assertEquals(
                    "hevc_strip/$display/$profile",
                    DvDecoderOptions(dolbyVision = true, p7Mode = "strip"),
                    GpuVoPolicy.dvDecoderOptions("hevc_strip", display, profile, nativeP5Decoder = false),
                )
            }
        }
        assertThrows(IllegalArgumentException::class.java) {
            GpuVoPolicy.dvDecoderOptions("sideways", displaySupportsDv = false, doviProfile = null, nativeP5Decoder = false)
        }
        for (mode in GpuVoPolicy.DV_CONVERSION_MODES) {
            GpuVoPolicy.dvDecoderOptions(mode, displaySupportsDv = false, doviProfile = null, nativeP5Decoder = false)
        }
    }

    /**
     * The defect that paired these two: P5 reached the plane with the DV decoder switched off, which renders it as
     * inverted plain HEVC. Whenever P5 is allowed to stay on the plane, the decoder must be driving it.
     */
    @Test
    fun p5IsNeverLeftOnThePlaneWithTheDvDecoderDisabled() {
        for (display in listOf(false, true)) {
            for (p5Decoder in listOf(false, true)) {
                val reshaping = GpuVoPolicy.needsDvReshaping(5, "auto", nativeP5Decoder = p5Decoder, softwareDecode = false)
                val options = GpuVoPolicy.dvDecoderOptions("auto", display, doviProfile = 5, nativeP5Decoder = p5Decoder)
                assertTrue("display=$display p5=$p5Decoder", reshaping || options.dolbyVision)
            }
        }
    }

    // "Disable Dolby Vision" (plezy#2543): a DV TV gets the HDR10 or HLG base layer the file carries, whatever the P7
    // conversion mode says.

    private fun disabledDvOptions(mode: String, profile: Int?, p5Decoder: Boolean): DvDecoderOptions {
        val routing = GpuVoPolicy.dvRouting(mode, displaySupportsDv = true, dvOutputAllowed = false)
        return GpuVoPolicy.dvDecoderOptions(routing.conversionMode, routing.displaySupportsDv, profile, nativeP5Decoder = p5Decoder)
    }

    @Test
    fun disablingDolbyVisionDecodesP7AndP8AsTheirBaseLayerOnADvDisplayInEveryMode() {
        for (mode in GpuVoPolicy.DV_CONVERSION_MODES) {
            for (p5Decoder in listOf(false, true)) {
                for (profile in listOf(7, 8)) {
                    val options = disabledDvOptions(mode, profile, p5Decoder)
                    assertEquals("$mode/P$profile/p5=$p5Decoder", DvDecoderOptions(dolbyVision = false, p7Mode = "strip"), options)
                    val decoderOptions = "dolby_vision=${if (options.dolbyVision) 1 else 0},dv_p7_mode=${options.p7Mode}"
                    val dvDecoders = listOf(
                        candidate("c2.vendor.dv.p7", profiles = listOf(GpuVoPolicy.DV_PROFILE_DVHE_DTB)),
                        candidate("c2.vendor.dv.p8", profiles = listOf(GpuVoPolicy.DV_PROFILE_DVHE_ST)),
                    )
                    assertEquals(DvRoute.BASE_LAYER, GpuVoPolicy.dvRoute(profile, decoderOptions, dvDecoders, "mediacodec", "mediacodec"))
                }
            }
        }
        // Allowed, the same display keeps P8 on the DV decoder.
        val allowed = GpuVoPolicy.dvRouting("auto", displaySupportsDv = true, dvOutputAllowed = true)
        assertTrue(GpuVoPolicy.dvDecoderOptions(allowed.conversionMode, allowed.displaySupportsDv, 8, nativeP5Decoder = false).dolbyVision)
    }

    @Test
    fun disablingDolbyVisionNeverStripsP5WhichHasNoBaseLayerToFallBackTo() {
        for (mode in GpuVoPolicy.DV_CONVERSION_MODES) {
            // A converting decoder keeps P5 on the DV path.
            assertTrue(mode, disabledDvOptions(mode, 5, p5Decoder = true).dolbyVision)
            // Without one, software decode reshapes it, from the prediction and from a decoder that landed in software
            // anyway.
            val routing = GpuVoPolicy.dvRouting(mode, displaySupportsDv = true, dvOutputAllowed = false)
            assertFalse(mode, disabledDvOptions(mode, 5, p5Decoder = false).dolbyVision)
            assertTrue(mode, GpuVoPolicy.needsDvReshaping(5, routing.conversionMode, nativeP5Decoder = false, softwareDecode = false))
            assertTrue(mode, GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, routing.conversionMode, hwdecCurrent = "no"))
        }
    }

    // A native decoder is one FFmpeg will open, which is narrower than what the device advertises: counting decoders
    // FFmpeg never asks for sent P5 to the plane as plain HEVC.

    private fun candidate(
        name: String,
        mime: String = GpuVoPolicy.DV_MIME,
        profiles: List<Int> = listOf(GpuVoPolicy.DV_PROFILE_DVHE_STN),
        isSoftwareOnly: Boolean = false,
    ) = DvDecoderCandidate(name, mime, profiles, isSoftwareOnly)

    private fun nativeP5Decoder(candidates: List<DvDecoderCandidate>) = GpuVoPolicy.nativeDvDecoder(candidates, 5)

    @Test
    fun aHardwareDvheStnDecoderUnderTheFfmpegMimeTypeIsTheNativeP5Path() {
        assertEquals("c2.amlogic.dolby-vision.dvhe.decoder", nativeP5Decoder(listOf(candidate("c2.amlogic.dolby-vision.dvhe.decoder"))))
        // Case-insensitive MIME match, as FFmpeg compares it.
        assertEquals("OMX.MTK.VIDEO.DECODER.DV", nativeP5Decoder(listOf(candidate("OMX.MTK.VIDEO.DECODER.DV", mime = "video/Dolby-Vision"))))
        // First match in list order, as FFmpeg takes it.
        assertEquals("first", nativeP5Decoder(listOf(candidate("first"), candidate("second"))))
    }

    @Test
    fun aDecoderRegisteredOnlyUnderAVendorDvMimeTypeIsNeverOpened() {
        // FFmpeg probes video/dolby-vision alone; a device whose DV decoder only answers to video/hevcdv "has DV" to
        // MediaCodecList and none to FFmpeg.
        assertNull(nativeP5Decoder(listOf(candidate("c2.vendor.dv.decoder", mime = "video/hevcdv"))))
        assertNull(nativeP5Decoder(listOf(candidate("c2.vendor.dv.decoder", mime = "video/dv_hevc"))))
    }

    @Test
    fun onlyTheExactProfileBitFfmpegProbesCounts() {
        // P7 (DvheDtb) and P8 (DvheSt) decoders convert nothing for single-layer P5.
        assertNull(nativeP5Decoder(listOf(candidate("c2.vendor.dv.decoder", profiles = listOf(0x40, 0x100)))))
        assertNull(nativeP5Decoder(listOf(candidate("c2.vendor.dv.decoder", profiles = emptyList()))))
        assertNull(nativeP5Decoder(emptyList()))
        // And a P5-only decoder is not what FFmpeg opens for a P8 stream.
        val p8Only = listOf(candidate("OMX.Nvidia.DOVI.decode", profiles = listOf(GpuVoPolicy.DV_PROFILE_DVHE_ST)))
        assertEquals("OMX.Nvidia.DOVI.decode", GpuVoPolicy.nativeDvDecoder(p8Only, 8))
        assertNull(GpuVoPolicy.nativeDvDecoder(p8Only, 5))
        // Single-layer P5 and P8 only; dual-layer P7's probes are dvRoute's.
        val all = listOf(candidate("c2.vendor.dv.decoder", profiles = listOf(0x20, 0x40, 0x100)))
        assertNull(GpuVoPolicy.nativeDvDecoder(all, 7))
        assertNull(GpuVoPolicy.nativeDvDecoder(all, null))
    }

    @Test
    fun softwareOnlyDecodersAreSkippedAsFfmpegSkipsThem() {
        // The API 29+ platform flag.
        assertNull(nativeP5Decoder(listOf(candidate("c2.android.dolby-vision.decoder", isSoftwareOnly = true))))
        // FFmpeg's own name blacklist on releases without the flag.
        for (name in listOf(
            "OMX.google.dolby-vision.decoder",
            "OMX.ffmpeg.dv.decoder",
            "OMX.SEC.hevc.sw.dec",
            "OMX.qcom.video.decoder.hevcswvdec",
        )) {
            assertNull(name, nativeP5Decoder(listOf(candidate(name))))
        }
        // The blacklist is exact where FFmpeg's is: Samsung hardware and the Qualcomm hardware HEVC decoder are not
        // software.
        assertEquals("OMX.SEC.hevc.dec", nativeP5Decoder(listOf(candidate("OMX.SEC.hevc.dec"))))
        assertEquals("OMX.qcom.video.decoder.hevc", nativeP5Decoder(listOf(candidate("OMX.qcom.video.decoder.hevc"))))
        // A hardware decoder later in the list still wins over an earlier software one.
        assertEquals(
            "c2.vendor.dv.decoder",
            nativeP5Decoder(listOf(candidate("OMX.google.dv", isSoftwareOnly = true), candidate("c2.vendor.dv.decoder"))),
        )
    }

    @Test
    fun aP5FileThatLandsInSoftwareDecodeNeedsReshapingWhateverWasPredicted() {
        // hwdec-current is the outcome; a predicted native decoder that failed to open leaves the base layer
        // unreshaped unless gpu-next takes it.
        assertTrue(GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, "auto", hwdecCurrent = "no"))
        assertTrue(GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, "auto", hwdecCurrent = "mediacodec-copy"))
        // Still on the hardware decoder: nothing to react to.
        assertFalse(GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, "auto", hwdecCurrent = "mediacodec"))
        assertFalse(GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, "auto", hwdecCurrent = null))
        // Base-layer-compatible profiles and explicit modes follow needsDvReshaping.
        assertFalse(GpuVoPolicy.softwareDecodeNeedsDvReshaping(8, "auto", hwdecCurrent = "no"))
        assertFalse(GpuVoPolicy.softwareDecodeNeedsDvReshaping(null, "auto", hwdecCurrent = "no"))
        assertFalse(GpuVoPolicy.softwareDecodeNeedsDvReshaping(5, "native", hwdecCurrent = "no"))
    }

    // The overlay's DV path reports what FFmpeg's dolby_vision_override does with the options mpv holds, not what the
    // routing policy asked for (plezy#2534).

    private val p8Decoder = candidate("c2.vendor.dv.decoder", profiles = listOf(GpuVoPolicy.DV_PROFILE_DVHE_ST))

    private fun route(
        profile: Int?,
        options: String? = "ndk_codec=0,async=1,dolby_vision=1,dv_p7_mode=auto",
        candidates: List<DvDecoderCandidate> = listOf(p8Decoder),
        hwdec: String? = "mediacodec",
        vo: String? = "mediacodec",
    ) = GpuVoPolicy.dvRoute(profile, options, candidates, hwdec, vo)

    @Test
    fun p8WithTheDvDecoderOffIsItsBaseLayerNotTheDvDecoder() {
        // auto on a display without DV writes dolby_vision=0: HEVC takes the HDR10 base layer even though the device
        // has a P8 decoder.
        assertEquals(DvRoute.BASE_LAYER, route(8, options = "ndk_codec=0,async=1,dolby_vision=0,dv_p7_mode=strip"))
        assertEquals(DvRoute.DV_DECODER, route(8))
    }

    @Test
    fun aDvDecoderFfmpegDoesNotOpenIsNeverReported() {
        assertEquals(DvRoute.BASE_LAYER, route(8, candidates = listOf(candidate("c2.vendor.dv", mime = "video/hevcdv", profiles = listOf(0x100)))))
        // Exact profile: a P5-only decoder takes P5 and nothing else.
        val p5Only = listOf(candidate("c2.vendor.dv"))
        assertEquals(DvRoute.BASE_LAYER, route(8, candidates = p5Only))
        assertEquals(DvRoute.DV_DECODER, route(5, candidates = p5Only))
        assertEquals(DvRoute.BASE_LAYER, route(8, candidates = listOf(candidate("OMX.google.dv", profiles = listOf(0x100)))))
    }

    @Test
    fun theLastDuplicateDecoderOptionWinsAsFfmpegAppliesIt() {
        // A user's own vd-lavc-o text follows the app's keys.
        assertEquals(DvRoute.BASE_LAYER, route(8, options = "dolby_vision=1,dv_p7_mode=auto,dolby_vision=0"))
        assertEquals(DvRoute.BASE_LAYER, route(8, options = "dolby_vision=1,dolby_vision=off"))
        assertEquals(DvRoute.DV_DECODER, route(8, options = "dolby_vision=0,dolby_vision=true"))
        // Unset is FFmpeg's default, dolby_vision=1.
        assertEquals(DvRoute.DV_DECODER, route(8, options = "ndk_codec=0"))
        assertEquals(DvRoute.DV_DECODER, route(8, options = null))
    }

    @Test
    fun softwareDecodeIsNeverTheDvDecoder() {
        // A predicted DV decoder that failed to open falls back to software.
        assertEquals(DvRoute.BASE_LAYER, route(8, hwdec = "no", vo = "gpu"))
        assertEquals(DvRoute.RESHAPED, route(5, hwdec = "no", vo = "gpu-next"))
        assertEquals(DvRoute.RESHAPED, route(8, hwdec = "no", vo = "gpu-next"))
        // Copied-back MediaCodec frames still come from the DV decoder, and carry no RPU metadata for gpu-next.
        assertEquals(DvRoute.DV_DECODER, route(8, hwdec = "mediacodec-copy", vo = "gpu-next"))
    }

    @Test
    fun p7FollowsDvP7ModeThroughTheNativeConvertedAndStrippedPaths() {
        val p7Decoder = candidate("c2.vendor.dv.p7", profiles = listOf(GpuVoPolicy.DV_PROFILE_DVHE_DTB))
        val both = listOf(p7Decoder, p8Decoder)
        assertEquals(DvRoute.DV_DECODER, route(7, options = "dolby_vision=1,dv_p7_mode=auto", candidates = both))
        assertEquals(DvRoute.DV_DECODER_P81, route(7, options = "dolby_vision=1,dv_p7_mode=auto", candidates = listOf(p8Decoder)))
        assertEquals(DvRoute.DV_DECODER_P81, route(7, options = "dolby_vision=1,dv_p7_mode=convert", candidates = both))
        assertEquals(DvRoute.DV_DECODER_P81, route(7, options = "dolby_vision=1,dv_p7_mode=2", candidates = both))
        assertEquals(DvRoute.DV_DECODER, route(7, options = "dolby_vision=1,dv_p7_mode=native", candidates = both))
        assertEquals(DvRoute.BASE_LAYER, route(7, options = "dolby_vision=1,dv_p7_mode=native", candidates = listOf(p8Decoder)))
        assertEquals(DvRoute.BASE_LAYER, route(7, options = "dolby_vision=1,dv_p7_mode=strip", candidates = both))
        assertEquals(DvRoute.BASE_LAYER, route(7, options = "dolby_vision=1,dv_p7_mode=auto", candidates = emptyList()))
        // dolby_vision=0 keeps P7 on its base layer whatever the mode says.
        assertEquals(DvRoute.BASE_LAYER, route(7, options = "dolby_vision=0,dv_p7_mode=convert", candidates = both))
    }

    @Test
    fun noRouteWithoutADoviRecordOrAnOpenDecoder() {
        assertNull(route(null))
        assertNull(route(8, hwdec = null))
        assertNull(route(8, hwdec = ""))
        // Profiles the HEVC override never re-routes decode their base layer.
        assertEquals(DvRoute.BASE_LAYER, route(10, candidates = listOf(candidate("c2.vendor.dv.av1", profiles = listOf(0x400)))))
    }

    // HDR to SDR.

    private val noHdr = IntArray(0)
    private val hdr10 = intArrayOf(Display.HdrCapabilities.HDR_TYPE_HDR10)
    private val hdr10Hlg = intArrayOf(Display.HdrCapabilities.HDR_TYPE_HDR10, Display.HdrCapabilities.HDR_TYPE_HLG)
    private val dvOnly = intArrayOf(Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION)

    private fun toneMaps(gamma: String?, display: IntArray, mode: String, sdkInt: Int, hdrOutput: Boolean = true, dvDecoder: Boolean = false) =
        GpuVoPolicy.needsToneMapping(gamma, display, dvDecoder, hdrOutput, mode, sdkInt)

    @Test
    fun theDisplaysFormatDecidesWhetherItTakesTheSignal() {
        // Under player, whatever the display doesn't take is tone-mapped in mpv.
        for (api in listOf(34, 35)) {
            assertTrue("a display without HDR, API $api", toneMaps("pq", noHdr, "player", api))
            assertTrue("HLG on an HDR10-only display, API $api", toneMaps("hlg", hdr10, "player", api))
            assertTrue("a PQ base layer on a DV-only display, API $api", toneMaps("pq", dvOnly, "player", api))
            assertFalse("HDR10 out to an HDR10 display, API $api", toneMaps("pq", hdr10, "player", api))
            assertFalse("HLG out to an HLG display, API $api", toneMaps("hlg", hdr10Hlg, "player", api))
            assertFalse("a DV decoder's picture on a DV display, API $api", toneMaps("pq", dvOnly, "player", api, dvDecoder = true))
        }
    }

    @Test
    fun noHdrOutputLeavesThePlaneOnlyWhereNothingCapsTheSurface() {
        for (mode in GpuVoPolicy.HDR_SDR_CONVERSION_MODES) {
            assertTrue("$mode below API 35: nothing caps the headroom", toneMaps("pq", hdr10, mode, 34, hdrOutput = false))
            assertFalse("$mode from API 35: the plane, its headroom capped", toneMaps("pq", hdr10, mode, 35, hdrOutput = false))
        }
    }

    @Test
    fun autoLeavesHdrTheDisplayDoesntTakeOnThePlaneFromAndroid9AndToneMapsInMpvBelowIt() {
        for (gamma in listOf("pq", "hlg")) {
            assertTrue(toneMaps(gamma, noHdr, "auto", 27))
            assertFalse(toneMaps(gamma, noHdr, "auto", 28))
        }
        assertFalse("HLG on an HDR10-only display from API 28", toneMaps("hlg", hdr10, "auto", 34))
    }

    @Test
    fun anExplicitModeOverridesTheApiLevel() {
        assertTrue(toneMaps("pq", noHdr, "player", 34))
        assertFalse(toneMaps("pq", noHdr, "device", 25))
        assertFalse(toneMaps("hlg", hdr10, "device", 25))
    }

    @Test
    fun anSdrSignalIsNeverToneMapped() {
        for (mode in GpuVoPolicy.HDR_SDR_CONVERSION_MODES) {
            // mpv reports no transfer before a file's first frame.
            for (gamma in listOf("bt.1886", "srgb", null, "")) {
                assertFalse("$mode/$gamma", toneMaps(gamma, noHdr, mode, 25, hdrOutput = false))
            }
        }
    }

    @Test
    fun anUnknownHdrConversionModeIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { toneMaps("pq", noHdr, "platform", 34) }
        assertThrows(IllegalArgumentException::class.java) { toneMaps("bt.1886", hdr10, "platform", 34) }
    }

    @Test
    fun hdrDroppedByTheHardwareDecoderIsCaughtOnlyAfterHdrWasSeen() {
        assertTrue(GpuVoPolicy.hardwareDroppedHdr(hdrSeen = true, gamma = "bt.1886", hwdecCurrent = "mediacodec"))
        assertFalse("SDR from the start", GpuVoPolicy.hardwareDroppedHdr(hdrSeen = false, gamma = "bt.1886", hwdecCurrent = "mediacodec"))
        assertFalse(GpuVoPolicy.hardwareDroppedHdr(hdrSeen = true, gamma = "hlg", hwdecCurrent = "mediacodec"))
        assertFalse("software decode reads the bitstream", GpuVoPolicy.hardwareDroppedHdr(hdrSeen = true, gamma = "bt.1886", hwdecCurrent = "no"))
        assertFalse("no output configured", GpuVoPolicy.hardwareDroppedHdr(hdrSeen = true, gamma = null, hwdecCurrent = "mediacodec"))
    }

    // The decode path.

    @Test
    fun onlyDirectMediacodecOutputCountsAsHardwareFrames() {
        // -copy also reads frames back into system memory.
        assertTrue(GpuVoPolicy.needsSoftwareRender("no"))
        assertTrue(GpuVoPolicy.needsSoftwareRender("mediacodec-copy"))
        assertFalse(GpuVoPolicy.needsSoftwareRender("mediacodec"))
        // Unreported until the decoder initializes: nothing to reshape yet.
        assertFalse(GpuVoPolicy.needsSoftwareRender(null))
        assertFalse(GpuVoPolicy.needsSoftwareRender(""))
    }

    @Test
    fun onlyAnAv1SessionAskingForHardwareDecodeOnBigOceanParksTheDecoderAcrossARebuild() {
        assertTrue(GpuVoPolicy.needsParkedRebuild("av1", "mediacodec,mediacodec-copy", bigOceanAv1 = true))
        assertTrue(GpuVoPolicy.needsParkedRebuild("av1", "mediacodec", bigOceanAv1 = true))
        // Other decoders survive being re-created inside the rebuild.
        assertFalse(GpuVoPolicy.needsParkedRebuild("av1", "mediacodec", bigOceanAv1 = false))
        assertFalse(GpuVoPolicy.needsParkedRebuild("hevc", "mediacodec", bigOceanAv1 = true))
        // A software session (a user setting or a per-file hold) never touches the hardware instance.
        assertFalse(GpuVoPolicy.needsParkedRebuild("av1", "no", bigOceanAv1 = true))
        assertFalse(GpuVoPolicy.needsParkedRebuild("av1", "", bigOceanAv1 = true))
        assertFalse(GpuVoPolicy.needsParkedRebuild("av1", null, bigOceanAv1 = true))
        assertFalse(GpuVoPolicy.needsParkedRebuild(null, "mediacodec", bigOceanAv1 = true))
    }

    private fun softwareDecode(codec: String?, profile: String?, high10: Boolean = false, main10: Boolean = false, av1: Boolean = false) =
        GpuVoPolicy.needsSoftwareDecode(codec, profile, hardwareHigh10 = high10, hardwareHevcMain10 = main10, hardwareAv1 = av1)

    @Test
    fun tenBitProfilesWithoutAHardwareProfileAreSoftwareDecodedUpFront() {
        assertTrue(softwareDecode("h264", "High 10"))
        assertTrue(softwareDecode("h264", "High 10 Intra"))
        assertTrue(softwareDecode("hevc", "Main 10"))
        assertEquals("gpu", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_CODEC_SW_DECODE)))
    }

    @Test
    fun hardwareSupportedAndUnrelatedStreamsKeepTheConfiguredDecoder() {
        // A decoder that advertises the profile gets to try.
        assertFalse(softwareDecode("h264", "High 10", high10 = true))
        assertFalse(softwareDecode("hevc", "Main 10", main10 = true))
        // 8-bit profiles, other codecs, and streams whose container carries no profile are not routed.
        assertFalse(softwareDecode("h264", "High"))
        assertFalse(softwareDecode("h264", "Constrained Baseline"))
        assertFalse(softwareDecode("hevc", "Main"))
        assertFalse(softwareDecode("h264", null))
        assertFalse(softwareDecode("hevc", null))
        assertFalse(softwareDecode("h264", ""))
        assertFalse(softwareDecode(null, "High 10"))
    }

    @Test
    fun av1BypassesSoftwareMediacodecEvenWithoutAReportedProfile() {
        assertTrue(softwareDecode("av1", "Main", high10 = true, main10 = true))
        assertTrue(softwareDecode("av1", null, high10 = true, main10 = true))
        assertTrue(softwareDecode("av1", ""))
        assertFalse(softwareDecode("av1", "Main", av1 = true))
    }

    // The per-file policies run inside on_preloaded, before mpv selects a track, so the track they decide for comes
    // from the pending selection.

    @Test
    fun autoSelectionFollowsMpvsPendingChoiceNotTrackListOrder() {
        // Cover art first, the default-flagged feature second: mpv picks 2.
        assertEquals(2L, GpuVoPolicy.pendingVideoTrackId("auto", "2", listOf(1L, 2L)))
        assertEquals(1L, GpuVoPolicy.pendingVideoTrackId("auto", "1", listOf(1L, 2L)))
    }

    @Test
    fun anExplicitVidAnswersOnItsOwn() {
        assertEquals(2L, GpuVoPolicy.pendingVideoTrackId("2", pendingVid = null, videoTrackIds = listOf(1L, 2L)))
        // A user's explicit choice is never re-selected, even when mpv would pick differently.
        assertEquals(1L, GpuVoPolicy.pendingVideoTrackId("1", "2", listOf(1L, 2L)))
        // No such track: mpv selects nothing, so nothing is routed.
        assertNull(GpuVoPolicy.pendingVideoTrackId("7", "2", listOf(1L, 2L)))
    }

    @Test
    fun noVideoSelectionYieldsNoTrack() {
        assertNull(GpuVoPolicy.pendingVideoTrackId("no", "1", listOf(1L, 2L)))
        assertNull(GpuVoPolicy.pendingVideoTrackId("auto", "no", listOf(1L, 2L)))
        assertNull(GpuVoPolicy.pendingVideoTrackId("auto", "1", emptyList()))
    }

    @Test
    fun withoutThePendingVidPropertyTheFirstTrackIsTheFallback() {
        assertEquals(1L, GpuVoPolicy.pendingVideoTrackId("auto", null, listOf(1L, 2L)))
        assertEquals(1L, GpuVoPolicy.pendingVideoTrackId(null, null, listOf(1L, 2L)))
        assertNull(GpuVoPolicy.pendingVideoTrackId("auto", null, emptyList()))
    }

    // The vo target.

    @Test
    fun noReasonsKeepTheVideoPlane() {
        assertNull(GpuVoPolicy.targetFor(emptySet()))
    }

    @Test
    fun everyReasonButDvReshapingTargetsGpu() {
        for (reason in listOf(
            GpuVoPolicy.REASON_SHADERS,
            GpuVoPolicy.REASON_FORMAT_REJECTED,
            GpuVoPolicy.REASON_SW_DECODE,
            GpuVoPolicy.REASON_CODEC_SW_DECODE,
            GpuVoPolicy.REASON_HWDEC_OFF,
            GpuVoPolicy.REASON_HDR_ASPECTS,
            GpuVoPolicy.REASON_TONE_MAP,
        )) {
            assertEquals(reason, "gpu", GpuVoPolicy.targetFor(setOf(reason)))
        }
        assertEquals("gpu", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_SHADERS, GpuVoPolicy.REASON_FORMAT_REJECTED)))
    }

    @Test
    fun dvReshapingTargetsGpuNextEvenAlongsideOtherReasons() {
        assertEquals("gpu-next", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_DV_RESHAPE)))
        assertEquals("gpu-next", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_FORMAT_REJECTED, GpuVoPolicy.REASON_DV_RESHAPE)))
        assertEquals("gpu-next", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_SW_DECODE, GpuVoPolicy.REASON_DV_RESHAPE)))
        assertEquals("gpu-next", GpuVoPolicy.targetFor(setOf(GpuVoPolicy.REASON_TONE_MAP, GpuVoPolicy.REASON_DV_RESHAPE)))
    }

    // The cheap render tier.

    @Test
    fun theCheapRenderTierNeedsAGlVoOnADriverWithoutNorm16() {
        assertTrue(GpuVoPolicy.needsCheapRenderTier(glVoActive = true, textureNorm16 = false))
        // The plane never scales in GL; a capable GPU keeps mpv's defaults.
        assertFalse(GpuVoPolicy.needsCheapRenderTier(glVoActive = false, textureNorm16 = false))
        assertFalse(GpuVoPolicy.needsCheapRenderTier(glVoActive = true, textureNorm16 = true))
    }

    @Test
    fun theCheapTierReplacesOnlyOptionsStillAtTheirMpvDefault() {
        for ((option, defaults) in GpuVoPolicy.MPV_DEFAULT_RENDER_OPTIONS) {
            for (default in defaults) assertTrue(option, GpuVoPolicy.isDefaultRenderOption(option, default))
            // A user's mpv.conf value, or an unreadable option, is left alone.
            assertFalse(option, GpuVoPolicy.isDefaultRenderOption(option, "ewa_lanczos"))
            assertFalse(option, GpuVoPolicy.isDefaultRenderOption(option, null))
        }
        // cscale's default is "inherit", which mpv 0.41 reads back as empty.
        assertTrue(GpuVoPolicy.isDefaultRenderOption("cscale", ""))
        assertFalse(GpuVoPolicy.isDefaultRenderOption("scale", ""))
        assertFalse(GpuVoPolicy.isDefaultRenderOption("tone-mapping", "bt.2390"))
    }

    // The decode stall watch.

    /** Playing, 42 s in, the decoder's last packet at 43.5 s: what a look sees while the file plays on. */
    private val playing = DecodeSample(
        position = 42.0,
        restarts = 2,
        videoReaderPts = 43.5,
        restartPending = false,
        paused = false,
        pausedForCache = false,
        eof = false,
    )

    @Test
    fun aDecoderStuckForTheLimitWithVideoWaitingEndsInSoftwareThenInAnError() {
        val n = DECODE_STALL_SECONDS
        assertEquals("just under the limit", DecodeStall.NONE, GpuVoPolicy.decodeStall(playing, n - 0.1, 8.0, hardwareAllowed = true))
        assertEquals("at the limit", DecodeStall.SOFTWARE, GpuVoPolicy.decodeStall(playing, n, 8.0, hardwareAllowed = true))
        assertEquals(
            "just enough video waiting",
            DecodeStall.SOFTWARE,
            GpuVoPolicy.decodeStall(playing, n + 0.1, DECODE_STALL_MIN_CACHE_SECONDS, hardwareAllowed = true),
        )
        assertEquals(
            "just too little video waiting: the source is the wait",
            DecodeStall.NONE,
            GpuVoPolicy.decodeStall(playing, n + 0.1, DECODE_STALL_MIN_CACHE_SECONDS - 0.1, hardwareAllowed = true),
        )
        assertEquals("software stuck too: the player's error", DecodeStall.FAIL, GpuVoPolicy.decodeStall(playing, n, 8.0, hardwareAllowed = false))
    }

    @Test
    fun aLoadOrSeekWaitingForItsFirstFrameStallsPausedOrNot() {
        val waiting = playing.copy(restartPending = true)
        assertEquals(DecodeStall.NONE, GpuVoPolicy.decodeStall(waiting, DECODE_STALL_SECONDS - 0.1, 8.0, hardwareAllowed = true))
        assertEquals(DecodeStall.SOFTWARE, GpuVoPolicy.decodeStall(waiting, DECODE_STALL_SECONDS, 8.0, hardwareAllowed = true))
        assertEquals(
            "mpv decodes a load's or a seek's first frame paused too",
            DecodeStall.SOFTWARE,
            GpuVoPolicy.decodeStall(waiting.copy(paused = true), DECODE_STALL_SECONDS, 8.0, hardwareAllowed = true),
        )
    }

    @Test
    fun nothingDueOrTheSourceAsTheWaitIsNeverAStall() {
        val long = DECODE_STALL_SECONDS * 100
        assertEquals("the user's pause", DecodeStall.NONE, GpuVoPolicy.decodeStall(playing.copy(paused = true), long, 8.0, hardwareAllowed = true))
        assertEquals(
            "buffering on a slow network",
            DecodeStall.NONE,
            GpuVoPolicy.decodeStall(playing.copy(pausedForCache = true), long, 8.0, hardwareAllowed = true),
        )
        assertEquals(
            "a seek into an empty cache",
            DecodeStall.NONE,
            GpuVoPolicy.decodeStall(playing.copy(restartPending = true), long, 0.0, hardwareAllowed = true),
        )
        assertEquals("an audio-only file: no video read ahead", DecodeStall.NONE, GpuVoPolicy.decodeStall(playing, long, null, hardwareAllowed = true))
        assertEquals("the end of the file", DecodeStall.NONE, GpuVoPolicy.decodeStall(playing.copy(eof = true), long, 8.0, hardwareAllowed = true))
    }
}
