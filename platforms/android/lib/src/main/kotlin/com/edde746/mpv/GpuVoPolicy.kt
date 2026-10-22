// Derived from Plezy's GpuVoPolicy.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import android.os.Build
import android.view.Display

/**
 * Pure policy for when an mpv session must leave the video plane (`vo=mediacodec`) for a GL video output, which one,
 * and how the per-file decode is routed before the decoder opens. Kept free of player and platform state so the
 * routing matrix stays testable: the app reads mpv's properties and the device's answers ([MediaCodecQuery],
 * [GlCapabilities], the display's HDR types) and passes them in.
 *
 * A session collects the reasons it currently has for a GL vo (the `REASON_*` names, as a set) and runs
 * [targetFor] of them. Issue numbers `plezy#N` are https://github.com/edde746/plezy/issues/N.
 */
object GpuVoPolicy {
    /**
     * Whether the decoder is handing mpv software frames, from `hwdec-current`: `no`, or `mediacodec-copy`, which reads
     * frames back into system memory. Unreported (null or blank) until the decoder initializes.
     *
     * The plane refuses every format but MediaCodec buffers, so a per-file decode fallback (AV1 on Tegra, Hi10 without
     * a profile match) has to move to a GL vo. Routing on this gets there as soon as the decoder settles;
     * [REASON_FORMAT_REJECTED] (mpv's `vo-format-rejected`) catches every other frame the plane refuses.
     */
    fun needsSoftwareRender(hwdecCurrent: String?): Boolean = !hwdecCurrent.isNullOrBlank() && hwdecCurrent != "mediacodec"

    /** Whether mpv's `video-params/gamma` is an HDR transfer. */
    fun isHdrTransfer(gamma: String?): Boolean = gamma == "pq" || gamma == "hlg"

    /** The `conversionMode` values of [needsToneMapping]: who converts HDR for a display that doesn't take it. */
    val HDR_SDR_CONVERSION_MODES: Set<String> = setOf("auto", "device", "player")

    /**
     * The first API level whose platform converts an HDR layer for a display without HDR output (Android 9). From P,
     * AOSP SurfaceFlinger moves a PQ or HLG layer to GPU composition when the hardware composer reports no HDR10 or
     * HLG and tone-maps it in RenderEngine (libtonemap from 13). 7.x and 8.x have no such path: HDR comes out washed
     * out (Fire OS 6, API 25, plezy#2121).
     */
    const val PLATFORM_HDR_TO_SDR_MIN_SDK = 28

    /**
     * Whether HDR frames ([gamma], `video-params/gamma`, an HDR transfer) leave the video plane for the GL vo, where
     * mpv tone-maps them to SDR, on API level [sdkInt].
     *
     * A display that takes their format keeps them on the plane: [displayHdrTypes] (`Display.HdrCapabilities`'s
     * `HDR_TYPE_*`, the active mode's) holds HDR10 for PQ or HLG for HLG, or Dolby Vision for a Dolby Vision decoder's
     * picture ([dolbyVisionDecoder]). The one exception is a playback without HDR output ([hdrOutput] false) below API
     * 35, where nothing caps the Surface's HDR headroom (`setDesiredHdrHeadroom`), so the plane would put HDR out. From
     * API 35 the plane stays with the headroom capped at SDR and the compositor tone-maps (the Pixel 7's composer does
     * it as an overlay).
     *
     * A display that doesn't take the format (no HDR at all, HLG on an HDR10-only display, a PQ base layer on a Dolby
     * Vision-only one) leaves the conversion to [conversionMode], one of [HDR_SDR_CONVERSION_MODES]: `player` tone-maps
     * in mpv, `device` keeps the plane and trusts the platform, `auto` keeps the plane from
     * [PLATFORM_HDR_TO_SDR_MIN_SDK] and tone-maps in mpv below it. Which is cheaper depends on the device:
     * - The platform converts in the hardware composer where it can, and costs no GPU time; the plane is what Media3,
     *   Kodi and VLC use for hardware-decoded HDR on SDR displays. A Box R (Mali-G31) shows 4K HDR10 at about 25 fps on
     *   the plane, where its Amlogic video layer converts, and 11 fps through the GL vo.
     * - Where the composer can't, SurfaceFlinger composites the HDR layer on the GPU at the display's resolution in
     *   every frame. On the Google TV Streamer (PowerVR GE9215, API 34, a 4K60 display without HDR) the layer goes to
     *   client composition, the app's own frames wait seconds for the GPU, and within a minute SurfaceFlinger reports
     *   their stuck fence as an ANR ("Buffer processing hung up due to stuck fence"), at 1080p as at 4K, while SDR
     *   video stays on the overlay. On the GL vo mpv tone-maps once per frame at the window's size and the composer
     *   overlays an SDR layer.
     *
     * Throws [IllegalArgumentException] for a [conversionMode] outside [HDR_SDR_CONVERSION_MODES].
     */
    fun needsToneMapping(
        gamma: String?,
        displayHdrTypes: IntArray,
        dolbyVisionDecoder: Boolean,
        hdrOutput: Boolean,
        conversionMode: String,
        sdkInt: Int,
    ): Boolean {
        require(conversionMode in HDR_SDR_CONVERSION_MODES) { "Invalid HDR-to-SDR conversion mode: $conversionMode" }
        if (!isHdrTransfer(gamma)) return false
        val displayTakes = when {
            dolbyVisionDecoder && Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION in displayHdrTypes -> true
            gamma == "pq" -> Display.HdrCapabilities.HDR_TYPE_HDR10 in displayHdrTypes
            else -> Display.HdrCapabilities.HDR_TYPE_HLG in displayHdrTypes
        }
        if (displayTakes) return !hdrOutput && sdkInt < Build.VERSION_CODES.VANILLA_ICE_CREAM
        return when (conversionMode) {
            "player" -> true
            "device" -> false
            else -> sdkInt < PLATFORM_HDR_TO_SDR_MIN_SDK
        }
    }

    /**
     * Whether the hardware decoder dropped the file's HDR signalling: `video-params` reported an HDR transfer for this
     * file ([hdrSeen]) and now, still on MediaCodec, reports [gamma] as SDR. On the plane MediaCodec tags the buffers
     * with the decoder's own colour aspects, and FFmpeg configures the decoder without the container's, so a decoder
     * that reports the bitstream's aspects only on the frames that carry them shows PQ video as washed-out SDR.
     * Measured on the Pixel 7: `c2.google.av1.decoder` with AV1 HDR10 reports BT.2020 PQ on the sequence-header frames
     * and BT.709 on every other one (settling on BT.709 after the first second). The software decoder reads the aspects
     * from the bitstream, so the file moves to software decode on the GL vo ([REASON_HDR_ASPECTS]).
     */
    fun hardwareDroppedHdr(hdrSeen: Boolean, gamma: String?, hwdecCurrent: String?): Boolean =
        hdrSeen && hwdecCurrent == "mediacodec" && gamma != null && !isHdrTransfer(gamma)

    /**
     * Single-layer Dolby Vision profile 5 (IPT-PQ-c2) has no compatible base layer: on a device without a decoder that
     * converts it, it decodes as plain HEVC with garbage colours, and the video plane applies no reshaping. gpu-next
     * (libplacebo) under software decode is the only Android path that composites the RPU metadata (plezy#1902).
     *
     * [doviProfile] comes from mpv's track-list (`dolby-vision-profile`, the bitstream's DOVI configuration record),
     * never from server or addon metadata, which mis-tags Dolby Vision routinely. Only the `auto` [conversionMode]
     * routes; the other [DV_CONVERSION_MODES] are explicit user choices. [nativeP5Decoder] is whether FFmpeg will open
     * a DV decoder for P5 ([nativeDvDecoder]); [softwareDecode] is a file that decodes in software from the start
     * (`hwdec=no`), which no decoder converts.
     */
    fun needsDvReshaping(doviProfile: Int?, conversionMode: String, nativeP5Decoder: Boolean, softwareDecode: Boolean): Boolean =
        doviProfile == 5 && conversionMode == "auto" && (softwareDecode || !nativeP5Decoder)

    /**
     * Whether a decoder that fell back to software is handing mpv an unreshaped P5 base layer: [needsDvReshaping]
     * predicted the native path (or another decoder failed to open), but `hwdec-current` ([hwdecCurrent]) says the
     * stream is not on MediaCodec, so nothing composites the RPU unless gpu-next does; plain `gpu` would scan the base
     * layer out as SDR BT.2020. Only the `auto` [conversionMode] routes, as in [needsDvReshaping].
     */
    fun softwareDecodeNeedsDvReshaping(doviProfile: Int?, conversionMode: String, hwdecCurrent: String?): Boolean =
        needsSoftwareRender(hwdecCurrent) && doviProfile == 5 && conversionMode == "auto"

    /**
     * A MediaCodec decoder as [nativeDvDecoder] sees it: the component name, one MIME type it serves, the profiles it
     * advertises for that type (`MediaCodecInfo.CodecProfileLevel` values) and the API 29+ `isSoftwareOnly` flag
     * (false below, where the platform does not classify). [MediaCodecQuery.dvDecoderCandidates] lists the device's.
     */
    data class DvDecoderCandidate(val name: String, val mime: String, val profiles: List<Int>, val isSoftwareOnly: Boolean)

    /** The only MIME type FFmpeg asks MediaCodecList for a Dolby Vision decoder under. */
    const val DV_MIME = "video/dolby-vision"

    /** `MediaCodecInfo.CodecProfileLevel.DolbyVisionProfileDvheStn`: single-layer profile 5. */
    const val DV_PROFILE_DVHE_STN = 0x20

    /** `MediaCodecInfo.CodecProfileLevel.DolbyVisionProfileDvheSt`: single-layer profile 8. */
    const val DV_PROFILE_DVHE_ST = 0x100

    /** `MediaCodecInfo.CodecProfileLevel.DolbyVisionProfileDvheDtb`: dual-layer profile 7. */
    const val DV_PROFILE_DVHE_DTB = 0x80

    /**
     * The decoder FFmpeg's `hevc_mediacodec` opens for a single-layer stream of [doviProfile] (5 or 8; null for any
     * other profile, dual-layer P7 included, whose probes [dvRoute] makes), or null when it opens none and the base
     * layer decodes as plain HEVC. Mirrors FFmpeg's `ff_AMediaCodecList_getCodecNameByType` exactly, because a probe
     * that counted decoders FFmpeg never asks for disagreed with it on real devices: only [DV_MIME] (`video/hevcdv` and
     * `video/dv_hevc` are never probed), an exact profile match ([DV_PROFILE_DVHE_STN], [DV_PROFILE_DVHE_ST]),
     * `isSoftwareOnly` skipped, and FFmpeg's software name blacklist skipped (`OMX.google*`, `OMX.ffmpeg*`,
     * `OMX.SEC*.sw.*`, `OMX.qcom.video.decoder.hevcswvdec`). First match in list order, as FFmpeg takes it.
     */
    fun nativeDvDecoder(candidates: List<DvDecoderCandidate>, doviProfile: Int?): String? {
        val profileBit = when (doviProfile) {
            5 -> DV_PROFILE_DVHE_STN
            8 -> DV_PROFILE_DVHE_ST
            else -> return null
        }
        return dvDecoderFor(candidates, profileBit)
    }

    /** FFmpeg's `dv_probe_profile`: the first hardware [DV_MIME] decoder advertising exactly [profileBit]. */
    private fun dvDecoderFor(candidates: List<DvDecoderCandidate>, profileBit: Int): String? = candidates.firstOrNull { candidate ->
        !candidate.isSoftwareOnly &&
            !isFfmpegSoftwareDecoderName(candidate.name) &&
            candidate.mime.equals(DV_MIME, ignoreCase = true) &&
            profileBit in candidate.profiles
    }?.name

    /** FFmpeg's `mediacodec_wrapper.c` software-decoder name blacklist, substring-matched as it does. */
    private fun isFfmpegSoftwareDecoderName(name: String): Boolean = name.contains("OMX.google") ||
        name.contains("OMX.ffmpeg") ||
        (name.contains("OMX.SEC") && name.contains(".sw.")) ||
        name == "OMX.qcom.video.decoder.hevcswvdec"

    /** What decodes a Dolby Vision file, as a performance overlay reports it ([dvRoute], plezy#2534). */
    enum class DvRoute(val id: String) {
        /** The [DV_MIME] decoder takes the stream as it is. */
        DV_DECODER("dv-decoder"),

        /** Profile 7's RPU is rewritten to 8.1 and the enhancement layer dropped, for a P8 [DV_MIME] decoder. */
        DV_DECODER_P81("dv-decoder-p81"),

        /** No DV decoder: the base layer decodes on its own and the RPU goes unused. */
        BASE_LAYER("base-layer"),

        /** Software decode on gpu-next, which applies the RPU itself (plezy#1902). */
        RESHAPED("reshaped"),
    }

    /**
     * The route the current file's video actually takes. Reports what FFmpeg's `dolby_vision_override` does with the
     * effective [decoderOptions] (mpv's `vd-lavc-o`, where a later duplicate key wins, as FFmpeg applies it), rather
     * than what the routing policy asked for: [dvDecoderOptions] only says whether the DV decoder *may* open.
     * [hwdecCurrent] is the outcome: a predicted DV decoder that fails to open makes mpv fall back to software decode,
     * which reads as the base layer or [DvRoute.RESHAPED] by [currentVo] (`current-vo`), never as the DV decoder.
     * MediaCodec output carries no RPU metadata, so only software frames can be reshaped.
     *
     * Null when the track has no DOVI record ([doviProfile]) or no decoder is open yet. P7 is predicted as dual-layer
     * and every DV file as carrying an RPU, because mpv exports neither `el_present_flag` nor `rpu_present_flag`;
     * FFmpeg uses the base layer when either assumption is false.
     */
    fun dvRoute(
        doviProfile: Int?,
        decoderOptions: String?,
        candidates: List<DvDecoderCandidate>,
        hwdecCurrent: String?,
        currentVo: String?,
    ): DvRoute? {
        if (doviProfile == null || hwdecCurrent.isNullOrBlank()) return null
        if (!hwdecCurrent.startsWith("mediacodec")) {
            return if (currentVo == "gpu-next") DvRoute.RESHAPED else DvRoute.BASE_LAYER
        }
        // FFmpeg's option defaults: dolby_vision=1, dv_p7_mode=auto.
        val dolbyVision = effectiveOption(decoderOptions, "dolby_vision")?.let(::avOptionBool) ?: true
        return when (doviProfile) {
            5, 8 -> if (dolbyVision && nativeDvDecoder(candidates, doviProfile) != null) DvRoute.DV_DECODER else DvRoute.BASE_LAYER
            7 -> {
                // dolby_vision=0 leaves profile 7 on its base layer whatever dv_p7_mode says.
                val p7Mode = if (dolbyVision) dvP7Mode(effectiveOption(decoderOptions, "dv_p7_mode")) else "strip"
                when {
                    p7Mode == "strip" -> DvRoute.BASE_LAYER
                    p7Mode != "convert" && dvDecoderFor(candidates, DV_PROFILE_DVHE_DTB) != null -> DvRoute.DV_DECODER
                    p7Mode != "native" && dvDecoderFor(candidates, DV_PROFILE_DVHE_ST) != null -> DvRoute.DV_DECODER_P81
                    else -> DvRoute.BASE_LAYER
                }
            }
            // FFmpeg re-routes HEVC single-layer P5 and P8 and dual-layer P7 only.
            else -> DvRoute.BASE_LAYER
        }
    }

    /** [key]'s value in an AVOption list such as mpv's `vd-lavc-o`: the last duplicate, as FFmpeg applies it. */
    private fun effectiveOption(options: String?, key: String): String? =
        options?.split(',')?.lastOrNull { it.substringBefore('=') == key }?.substringAfter('=', "")

    /** An `AV_OPT_TYPE_BOOL` value as FFmpeg parses it. */
    private fun avOptionBool(value: String): Boolean = when (value.lowercase()) {
        "false", "n", "no", "disable", "disabled", "off" -> false
        else -> value.toIntOrNull()?.let { it != 0 } ?: true
    }

    /** `dv_p7_mode` by constant name or its integer value. */
    private fun dvP7Mode(value: String?): String = when (value?.lowercase()) {
        "native", "1" -> "native"
        "convert", "2" -> "convert"
        "strip", "3" -> "strip"
        else -> "auto"
    }

    /** `dolby_vision` and `dv_p7_mode` for FFmpeg's `vd-lavc-o`. */
    data class DvDecoderOptions(val dolbyVision: Boolean, val p7Mode: String)

    /**
     * The Dolby Vision conversion modes [dvDecoderOptions] accepts: `auto` decides from the device and the file;
     * `disabled` and `native` hand every profile to the DV decoder with P7 as it is; `dv81` converts P7 to 8.1; `hevc`
     * and `hevc_strip` strip P7 to its base layer.
     */
    val DV_CONVERSION_MODES: Set<String> = setOf("auto", "disabled", "native", "dv81", "hevc", "hevc_strip")

    /**
     * How the Dolby Vision decoder is driven for the file whose pending video track carries [doviProfile] (null when
     * the bitstream has no DOVI record, or before any file is loaded) under [conversionMode]. Decided per file, because
     * `dolby_vision=1` sends every single-layer profile (5 and 8) to the DV decoder and the right answer differs by
     * profile. Paired with [needsDvReshaping]: `dolby_vision=0` bypasses the DV decoder, so if this says no for P5
     * while that says no reshaping, the P5 base layer reaches the plane as plain HEVC with inverted colour.
     *
     * Only `auto` reads the device. With a DV display ([displaySupportsDv]) the DV decoder takes every profile. Without
     * one, only P5 goes to the DV decoder, and only when FFmpeg will open one ([nativeP5Decoder]): P5 has no compatible
     * base layer, so a converting decoder is its only hardware path (measured on
     * `c2.amlogic.dolby-vision.dvhe.decoder` against an HDR10-only sink, plezy#2290). P8 decodes as plain HEVC instead:
     * its base layer stands on its own, and what a DV decoder converts it to without a DV sink is vendor-defined (the
     * Shield's `OMX.Nvidia.DOVI.decode` emits SDR, which scanned out muted with no HDR hand-off, plezy#2416).
     * Dual-layer P7 strips to its base layer either way.
     *
     * Throws [IllegalArgumentException] for a mode outside [DV_CONVERSION_MODES].
     */
    fun dvDecoderOptions(conversionMode: String, displaySupportsDv: Boolean, doviProfile: Int?, nativeP5Decoder: Boolean): DvDecoderOptions =
        when (conversionMode) {
            "auto" -> when {
                displaySupportsDv -> DvDecoderOptions(dolbyVision = true, p7Mode = "auto")
                else -> DvDecoderOptions(dolbyVision = doviProfile == 5 && nativeP5Decoder, p7Mode = "strip")
            }
            "disabled", "native" -> DvDecoderOptions(dolbyVision = true, p7Mode = "native")
            "dv81" -> DvDecoderOptions(dolbyVision = true, p7Mode = "convert")
            "hevc", "hevc_strip" -> DvDecoderOptions(dolbyVision = true, p7Mode = "strip")
            else -> throw IllegalArgumentException("Invalid DV conversion mode: $conversionMode")
        }

    /** The conversion mode and display answer the Dolby Vision routing decides with; see [dvRouting]. */
    data class DvRouting(val conversionMode: String, val displaySupportsDv: Boolean)

    /**
     * What [dvDecoderOptions], [needsDvReshaping] and [softwareDecodeNeedsDvReshaping] are asked with, given the
     * user's [conversionMode], whether the display takes Dolby Vision ([displaySupportsDv]), and whether Dolby Vision
     * output is allowed ([dvOutputAllowed]: false under a "Disable Dolby Vision" setting, plezy#2543). With Dolby
     * Vision disabled the session routes as `auto` on a display without Dolby Vision, whatever [conversionMode] says:
     * single-layer P8 decodes as plain HEVC and P7 strips to its base layer, so the TV receives the HDR10 (or HLG) the
     * file carries instead of a Dolby Vision signal. P5 has no compatible base layer and keeps that same route: the DV
     * decoder when the device has one, which still drives a Dolby Vision TV in Dolby Vision, else software decode with
     * gpu-next reshaping. Stripping P5 instead is the wrong-colour result Kodi's equivalent switch produces
     * (xbmc#26607).
     */
    fun dvRouting(conversionMode: String, displaySupportsDv: Boolean, dvOutputAllowed: Boolean): DvRouting =
        if (dvOutputAllowed) DvRouting(conversionMode, displaySupportsDv) else DvRouting("auto", displaySupportsDv = false)

    /**
     * Select native software decoding before opening a decoder when hardware cannot serve the stream. H.264 High 10
     * (plezy#2065) and HEVC Main 10 need an advertised hardware profile ([MediaCodecQuery.hardwareAvcHigh10],
     * [MediaCodecQuery.hardwareHevcMain10]): a decoder without one refuses the stream, renders garbage, or (the
     * Android emulator's `c2.goldfish.hevc.decoder` under Main 10) fails the first frame with MediaCodec error 14. AV1
     * without a hardware decoder ([MediaCodecQuery.hardwareAv1]) goes to dav1d directly rather than through a software
     * MediaCodec component (`c2.android.av1*`), which only adds a process hop and a copy. Tensor's
     * `c2.google.av1.decoder` is hardware and stays on this path; its rebuild hazard is [needsParkedRebuild]'s
     * (plezy#2272).
     *
     * [codec] and [codecProfile] come from mpv's pending video track ([pendingVideoTrackId]). This repository's mpv
     * publishes the H.264 and HEVC profile at load, from a Matroska or MP4 track's avcC or hvcC record or from FFmpeg's
     * stream probing (MPEG-TS); where neither gives one, [codecProfile] is null here and the decoder's own fallback
     * decides.
     */
    fun needsSoftwareDecode(
        codec: String?,
        codecProfile: String?,
        hardwareHigh10: Boolean,
        hardwareHevcMain10: Boolean,
        hardwareAv1: Boolean,
    ): Boolean = when (codec) {
        "h264" -> !hardwareHigh10 && codecProfile?.startsWith("High 10") == true
        "hevc" -> !hardwareHevcMain10 && codecProfile?.startsWith("Main 10") == true
        "av1" -> !hardwareAv1
        else -> false
    }

    /**
     * Whether a video-chain rebuild (a vo switch, the plane to a GL renderer or back, or a surface handoff while a GL
     * renderer is live) must run with the video track deselected. mpv re-creates the decoder inside that rebuild, and
     * Tensor's BigOcean AV1 service (`c2.google.av1.decoder`; [bigOceanAv1] is [MediaCodecQuery.bigOceanAv1]) crashes
     * when the next instance starts while the previous one is still shutting down; mpv then lands on
     * `mediacodec-copy` or software, and the plane can show neither (plezy#2272). Deselecting first closes the old
     * instance, the rebuild runs without a decoder, and re-selecting creates the next one against the finished output.
     * Only an AV1 session that asks for hardware decoding on that decoder pays the extra track switch. A surface
     * handoff on the plane is not a rebuild: `vo=mediacodec` repoints the running decoder at the new Surface in place.
     * [codec] is the current video track's codec; [hwdec] is the `hwdec` option (not `hwdec-current`, which lags a
     * freshly re-selected decoder).
     */
    fun needsParkedRebuild(codec: String?, hwdec: String?, bigOceanAv1: Boolean): Boolean =
        bigOceanAv1 && codec == "av1" && !hwdec.isNullOrBlank() && hwdec != "no"

    /**
     * The video track the per-file policies ([needsDvReshaping], [needsSoftwareDecode]) decide for, or null when no
     * video track will be selected. They run inside `on_preloaded`, where the track list is complete but nothing is
     * selected yet, so "the selected track" does not exist: `vid` still reads the option value, and mpv's own
     * selection (default and forced flags, `--vlang`, attached pictures skipped) runs only after the hook. [vid] is the
     * `vid` property: `no` and an explicit id are authoritative on their own; `auto` defers to [pendingVid], the
     * `pending-vid` property this repository's mpv adds, which runs that selection ahead of time. A null [pendingVid]
     * means a libmpv without that property, and the first of [videoTrackIds] (track-list order) is the best guess
     * left, wrong for a file whose first video track is not the one mpv picks.
     */
    fun pendingVideoTrackId(vid: String?, pendingVid: String?, videoTrackIds: List<Long>): Long? {
        val requested = when {
            vid == null || vid == "auto" -> pendingVid ?: return videoTrackIds.firstOrNull()
            else -> vid
        }
        val id = requested.toLongOrNull() ?: return null
        return id.takeIf { it in videoTrackIds }
    }

    /**
     * The vo a session with these active [reasons] should run, or null for the video plane. Dolby Vision reshaping
     * ([REASON_DV_RESHAPE]) is the one reason that needs gpu-next, since libplacebo is what composites the RPU;
     * everything else takes `gpu`, the battle-tested GLES renderer on the Android device zoo. (The magenta field
     * gpu-next used to render for 10-bit software frames on Tegra was its AV1 film grain shader overrunning the
     * driver's uniform register budget; grain is decoder-applied now, but gpu-next buys this path nothing over `gpu`.)
     */
    fun targetFor(reasons: Set<String>): String? = when {
        reasons.isEmpty() -> null
        REASON_DV_RESHAPE in reasons -> "gpu-next"
        else -> "gpu"
    }

    /**
     * Whether a GL vo session should drop to the cheap render tier ([CHEAP_RENDER_OPTIONS]): bilinear scalers, no
     * dither, and a tone-mapping curve without bt.2390's round trip through PQ. Keyed on `GL_EXT_texture_norm16` being
     * absent ([GlCapabilities.textureNorm16]), which on Android singles out the low-end TV class whose texture units
     * cannot afford a second full-resolution pass at 1080p: on an S905X4 (Mali-G31) mpv's default lanczos chroma pass
     * alone runs the frame over budget and bilinear brings it back under, while every norm16-capable GPU measured
     * renders the whole default ladder in a few milliseconds. The Android emulator's SwiftShader GLES is in the same
     * class: with mpv's default ladder its output is black, with this tier it renders (API 36 emulator). The curve: the
     * Google TV Streamer's PowerVR GE9215 dropped 118 of 288 frames of a 4K HDR10 remux under bt.2390 (mpv's `auto`)
     * with the tier's scalers, and 11 under mobius, which keeps in-range colours as they are. The video plane never
     * scales or tone-maps in GL, so sessions on it are untouched.
     */
    fun needsCheapRenderTier(glVoActive: Boolean, textureNorm16: Boolean): Boolean = glVoActive && !textureNorm16

    /** mpv option -> value for the cheap render tier, set only where [isDefaultRenderOption] (a user's value wins). */
    val CHEAP_RENDER_OPTIONS: Map<String, String> = linkedMapOf(
        "scale" to "bilinear",
        "cscale" to "bilinear",
        "dscale" to "bilinear",
        "dither" to "no",
        "tone-mapping" to "mobius",
    )

    /**
     * The values mpv reports for [CHEAP_RENDER_OPTIONS] when nothing set them; anything else is a deliberate choice and
     * stays. `cscale` inherits `scale` by default, which the property reads back as an empty string (mpv 0.41) or
     * `inherit`.
     */
    internal val MPV_DEFAULT_RENDER_OPTIONS: Map<String, Set<String>> = mapOf(
        "scale" to setOf("lanczos"),
        "cscale" to setOf("", "inherit"),
        "dscale" to setOf("hermite"),
        "dither" to setOf("fruit"),
        "tone-mapping" to setOf("auto"),
    )

    /** Whether [value] is what mpv reports for [option] by default. */
    fun isDefaultRenderOption(option: String, value: String?): Boolean =
        value != null && MPV_DEFAULT_RENDER_OPTIONS[option]?.contains(value) == true

    /** What a session does about the current file's video decoder at one look ([decodeStall]). */
    enum class DecodeStall { NONE, SOFTWARE, FAIL }

    /**
     * One look at the current file for [decodeStall], all from mpv: the shown frame's position ([position],
     * `time-pos`), the first frames counted so far ([restarts], `playback-restart`), the pts of the packet the video
     * decoder took last ([videoReaderPts], `demuxer-cache-state`'s video `reader-pts`), and what decides whether a
     * frame is due: a load or a seek waiting for its first frame ([restartPending], `seeking`), a pause (the user's, or
     * the app's on a lost Surface: [paused], `pause`), buffering ([pausedForCache], `paused-for-cache`) and the end
     * ([eof], `eof-reached`). The app counts how long a look stays the same: a frame, a packet taken, a pause or a
     * resume starts the count again.
     */
    data class DecodeSample(
        val position: Double,
        val restarts: Int,
        val videoReaderPts: Double?,
        val restartPending: Boolean,
        val paused: Boolean,
        val pausedForCache: Boolean,
        val eof: Boolean,
    )

    /**
     * Whether the current file's video decoder is stuck: [sample] has stayed the same for [unchangedSeconds] and the
     * demuxer holds [videoCacheSeconds] of video packets for the decoder. A decoder error MediaCodec reports reaches
     * mpv's own hardware-decoding fallback, across a flush too (this repository's FFmpeg keeps it). One it never
     * reports doesn't, and the decoder takes input and never answers: the emulator's `c2.goldfish.hevc.decoder` fails a
     * frame, and when a flush (a seek, or the refresh seek of a `vo` switch) comes first, codec2 drops that error with
     * the flushed work, so MediaCodec holds every input slot and reports nothing, and FFmpeg waits. That can happen at
     * the open, after a seek or mid-file.
     *
     * A stall: no new frame and no packet taken for [DECODE_STALL_SECONDS] while a frame is due (a load or a seek
     * waiting for its first frame, which mpv decodes paused too, or playback not paused), with at least
     * [DECODE_STALL_MIN_CACHE_SECONDS] of video packets waiting. Never one: the user's pause once the first frame is
     * shown, buffering (`paused-for-cache`, or less than [DECODE_STALL_MIN_CACHE_SECONDS] of video ahead: a slow
     * network, a seek into an empty cache), no video read ahead ([videoCacheSeconds] null: an audio-only file, cover
     * art, a video stream that has ended) and the end of the file. A slow decoder (software, or an exact seek decoding
     * up to its target) keeps taking packets, so it never counts; a video whose frames are [DECODE_STALL_SECONDS] apart
     * would.
     *
     * A stuck decoder that may be hardware ([hardwareAllowed]: the file's `hwdec` is not `no`) gives way to software
     * decode on the GL vo ([DecodeStall.SOFTWARE]); a software one fails the file ([DecodeStall.FAIL]).
     */
    fun decodeStall(sample: DecodeSample, unchangedSeconds: Double, videoCacheSeconds: Double?, hardwareAllowed: Boolean): DecodeStall = when {
        sample.eof || sample.pausedForCache || (sample.paused && !sample.restartPending) -> DecodeStall.NONE
        videoCacheSeconds == null || videoCacheSeconds < DECODE_STALL_MIN_CACHE_SECONDS -> DecodeStall.NONE
        unchangedSeconds < DECODE_STALL_SECONDS -> DecodeStall.NONE
        hardwareAllowed -> DecodeStall.SOFTWARE
        else -> DecodeStall.FAIL
    }

    /** Far beyond a working decoder's first frame: under 4 s on the emulator through every fallback, in software. */
    const val DECODE_STALL_SECONDS = 10.0

    /** Seconds of video packets ahead that rule the source out as what the decoder waits for. */
    const val DECODE_STALL_MIN_CACHE_SECONDS = 1.0

    /** P5 to reshape on gpu-next: [needsDvReshaping], [softwareDecodeNeedsDvReshaping]. */
    const val REASON_DV_RESHAPE = "dv-reshape"

    /** User shaders (`glsl-shaders`), which the video plane cannot render. */
    const val REASON_SHADERS = "shaders"

    /** Frames the plane cannot show (software or copied ones, a filter's output): mpv's `vo-format-rejected`. */
    const val REASON_FORMAT_REJECTED = "format-rejected"

    /** The decoder settled on software frames ([needsSoftwareRender]), or a [decodeStall] moved the file there. */
    const val REASON_SW_DECODE = "sw-decode"

    /** A codec or profile no hardware decoder serves: [needsSoftwareDecode]. */
    const val REASON_CODEC_SW_DECODE = "codec-sw-decode"

    /** The file's `hwdec` is `no` (hardware decoding turned off): software decode from the start. */
    const val REASON_HWDEC_OFF = "hwdec-off"

    /** Per file, a vo reason and an `hwdec=no` hold: see [hardwareDroppedHdr]. */
    const val REASON_HDR_ASPECTS = "hdr-aspects"

    /**
     * HDR video mpv tone-maps on the GL vo ([needsToneMapping]). Decided on each file's first `video-params`; until
     * then a new file keeps the previous file's route.
     */
    const val REASON_TONE_MAP = "tone-map"

    /**
     * Not a vo reason: the per-file `hwdec=no` hold once mpv decodes the file in software although hardware decoding
     * was allowed (its own fallback after hardware decode errors, or a codec no hardware decoder takes), or after a
     * [decodeStall]. The GL vo's rebuilt chain then opens the software decoder instead of probing MediaCodec again.
     */
    const val REASON_HW_DECODE_FAILURE = "hw-decode-failure"
}
