// Derived from Plezy's MediaCodecQuery.kt (https://github.com/edde746/plezy, GPL-3.0); see
// platforms/android/lib/NOTICE.
package com.edde746.mpv

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.os.Build
import android.util.Log
import java.util.Locale

/**
 * Hardware decoder lookup and classification: the inputs of the per-file decode routing ([GpuVoPolicy]) and of a
 * decoder's operating rate. The codec list is static, so the answers held here are read once per process.
 */
object MediaCodecQuery {
    private const val TAG = "MediaCodecQuery"

    /**
     * Whether a hardware `video/avc` decoder advertises H.264 High 10 (Hi10P). Decoders that do not advertise it either
     * refuse the stream (what mpv's and FFmpeg's MediaCodec path sees) or, on some SoCs, accept it and render garbage.
     */
    val hardwareAvcHigh10: Boolean by lazy {
        hasHardwareProfile("video/avc", MediaCodecInfo.CodecProfileLevel.AVCProfileHigh10)
    }

    /** Whether a hardware `video/hevc` decoder advertises Main 10 (plain or with HDR10/HDR10+ metadata). */
    val hardwareHevcMain10: Boolean by lazy {
        hasHardwareProfile(
            "video/hevc",
            MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10,
            MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10,
            MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10Plus,
        )
    }

    /** Whether a hardware AV1 decoder exists; software MediaCodec AV1 components do not count. */
    val hardwareAv1: Boolean by lazy { findHardwareDecoder("video/av01") != null }

    /**
     * Whether the hardware AV1 decoder is Tensor's BigOcean block (`c2.google.av1.decoder`, Pixel 6 and later). Its
     * vendor service dies when a new instance starts while the previous one is still being torn down;
     * [GpuVoPolicy.needsParkedRebuild] keeps the two apart.
     */
    val bigOceanAv1: Boolean by lazy {
        findHardwareDecoder("video/av01")?.name?.lowercase(Locale.ROOT)?.startsWith("c2.google.av1") == true
    }

    /**
     * Every decoder registered under a Dolby Vision MIME type, in MediaCodecList order, for
     * [GpuVoPolicy.nativeDvDecoder] and [GpuVoPolicy.dvRoute]. FFmpeg only asks for [GpuVoPolicy.DV_MIME]; the vendor
     * aliases are listed so a routing log shows a decoder the device has but FFmpeg never opens. A type whose
     * capabilities cannot be queried is dropped, as FFmpeg drops it; a codec list that cannot be read gives none.
     */
    val dvDecoderCandidates: List<GpuVoPolicy.DvDecoderCandidate> by lazy {
        try {
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filterNot { it.isEncoder }.flatMap { info ->
                info.supportedTypes.filter { it.lowercase(Locale.ROOT) in DV_MIME_TYPES }.mapNotNull { type ->
                    val profiles = try {
                        info.getCapabilitiesForType(type).profileLevels.map { it.profile }
                    } catch (e: IllegalArgumentException) {
                        Log.w(TAG, "Failed to query ${info.name} capabilities for $type", e)
                        return@mapNotNull null
                    }
                    // isSoftwareOnly exists from API 29; FFmpeg consults it only there too.
                    val softwareOnly = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && info.isSoftwareOnly
                    GpuVoPolicy.DvDecoderCandidate(info.name, type, profiles, softwareOnly)
                }
            }
        } catch (e: RuntimeException) {
            Log.w(TAG, "Failed to enumerate Dolby Vision decoders", e)
            emptyList()
        }
    }

    private val DV_MIME_TYPES = setOf(GpuVoPolicy.DV_MIME, "video/hevcdv", "video/dv_hevc")

    /**
     * Codecs whose support is decided by hardware: both have a software decoder behind them, but a software HEVC or
     * AV1 decode on phone- and TV-class hardware drops frames.
     */
    private val HARDWARE_GATED_VIDEO_MIME_TYPES = mapOf(
        "hevc" to "video/hevc",
        "av1" to "video/av01",
    )

    /** Codec name (`hevc`, `av1`) -> whether a hardware decoder serves it, given [hardwareMimeTypes]. */
    fun hardwareVideoDecodeSupport(hardwareMimeTypes: Set<String> = hardwareDecoderMimeTypes()): Map<String, Boolean> =
        HARDWARE_GATED_VIDEO_MIME_TYPES.mapValues { (_, mimeType) -> mimeType in hardwareMimeTypes }

    /** Every MIME type a hardware decoder serves, lowercased, from one walk of the codec list. */
    private fun hardwareDecoderMimeTypes(): Set<String> {
        val mimeTypes = HashSet<String>()
        for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
            if (info.isEncoder || !isHardwareAccelerated(info)) continue
            for (type in info.supportedTypes) mimeTypes.add(type.lowercase(Locale.ROOT))
        }
        return mimeTypes
    }

    private fun hasHardwareProfile(mimeType: String, vararg profiles: Int): Boolean =
        findHardwareDecoder(mimeType) { info, type ->
            val levels = try {
                info.getCapabilitiesForType(type).profileLevels
            } catch (e: IllegalArgumentException) {
                return@findHardwareDecoder false
            }
            levels.any { it.profile in profiles }
        } != null

    /** The first hardware decoder of [codecKind]'s list serving [mimeType] that [predicate] accepts, or null. */
    fun findHardwareDecoder(
        mimeType: String,
        codecKind: Int = MediaCodecList.REGULAR_CODECS,
        predicate: (MediaCodecInfo, String) -> Boolean = { _, _ -> true },
    ): MediaCodecInfo? {
        for (info in MediaCodecList(codecKind).codecInfos) {
            if (info.isEncoder || !isHardwareAccelerated(info)) continue
            for (type in info.supportedTypes) {
                if (type.equals(mimeType, ignoreCase = true) && predicate(info, type)) return info
            }
        }
        return null
    }

    /** The MediaFormat MIME type for an mpv/FFmpeg codec name a hardware decoder may serve; null for any other. */
    fun mimeTypeForCodec(codec: String?): String? = when (codec?.lowercase(Locale.ROOT)) {
        "h264" -> "video/avc"
        "hevc" -> "video/hevc"
        "av1" -> "video/av01"
        "vp9" -> "video/x-vnd.on2.vp9"
        "vp8" -> "video/x-vnd.on2.vp8"
        "mpeg2video" -> "video/mpeg2"
        "mpeg4" -> "video/mp4v-es"
        else -> null
    }

    private val PERFORMANCE_POINT_RATES = intArrayOf(240, 120, 90, 60, 50, 30, 25, 24)

    /**
     * The highest frame rate the hardware decoder for [mimeType] advertises at [width]×[height]: the manufacturer's
     * performance points on API 31+, the capability range below it. Null when there is no hardware decoder, the size
     * is outside what it declares, or the platform answers nothing; the caller then applies its own ceiling.
     */
    fun maxDecoderFrameRate(mimeType: String, width: Int, height: Int): Int? {
        if (width <= 0 || height <= 0) return null
        val info = findHardwareDecoder(mimeType) ?: return null
        val video = try {
            info.getCapabilitiesForType(mimeType).videoCapabilities
        } catch (e: IllegalArgumentException) {
            return null
        } ?: return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val points = video.supportedPerformancePoints
            if (!points.isNullOrEmpty()) {
                // The platform exposes only coverage, not a point's own rate: ask for the rates a declaration would
                // use, highest first.
                for (rate in PERFORMANCE_POINT_RATES) {
                    val wanted = MediaCodecInfo.VideoCapabilities.PerformancePoint(width, height, rate)
                    if (points.any { it.covers(wanted) }) return rate
                }
            }
        }
        return try {
            video.getSupportedFrameRatesFor(width, height).upper.toInt().takeIf { it > 0 }
        } catch (e: IllegalArgumentException) {
            null
        }
    }

    /** Whether [info] is a hardware decoder on this device's API level, by the rule of the overload below. */
    fun isHardwareAccelerated(info: MediaCodecInfo): Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        isHardwareAccelerated(Build.VERSION.SDK_INT, info.isHardwareAccelerated, info.name)
    } else {
        // MediaCodecInfo.isHardwareAccelerated() does not exist before API 29: the call stays inside the version gate
        // so older releases (Fire OS among them) do not fail with NoSuchMethodError.
        isHardwareAccelerated(Build.VERSION.SDK_INT, false, info.name)
    }

    /**
     * API 29 added the manufacturer's classification ([platformReportsHardware]), but some Codec2 builds still flag
     * known software components as hardware: from there both signals must agree. Older releases have only the
     * component [name].
     */
    internal fun isHardwareAccelerated(sdkInt: Int, platformReportsHardware: Boolean, name: String): Boolean =
        if (sdkInt >= Build.VERSION_CODES.Q) platformReportsHardware && !isSoftwareCodecName(name) else !isSoftwareCodecName(name)

    /**
     * Component names Android and FFmpeg ship as software. `c2.google.*` is not one of them: it is the Tensor vendor
     * prefix (`c2.google.av1.decoder` is the BigOcean hardware AV1 block), and API 29's platform flag classifies it.
     * Google's software components are `c2.android.*`.
     */
    internal fun isSoftwareCodecName(name: String): Boolean {
        val normalized = name.lowercase(Locale.ROOT)
        return normalized.startsWith("omx.google.") ||
            normalized.startsWith("omx.ffmpeg.") ||
            normalized.startsWith("c2.android.") ||
            normalized.startsWith("c2.ffmpeg.") ||
            normalized.contains(".sw.")
    }
}
