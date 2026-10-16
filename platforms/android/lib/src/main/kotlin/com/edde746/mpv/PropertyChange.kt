package com.edde746.mpv

/**
 * A change of an observed property, in the [PropertyFormat] it was observed with. [sourceId] is the playlist
 * entry (`playlist_entry_id`) the change belongs to: the entry of the last start-file before it, or null before
 * the first one.
 */
sealed interface PropertyChange {
    val name: String
    val sourceId: Long?

    /** The property changed to no value, or was observed with [PropertyFormat.None]. */
    data class None(
        override val name: String,
        override val sourceId: Long?,
    ) : PropertyChange

    data class Flag(
        override val name: String,
        val value: Boolean,
        override val sourceId: Long?,
    ) : PropertyChange

    data class Int64(
        override val name: String,
        val value: Long,
        override val sourceId: Long?,
    ) : PropertyChange

    data class Double(
        override val name: String,
        val value: kotlin.Double,
        override val sourceId: Long?,
    ) : PropertyChange

    data class Str(
        override val name: String,
        val value: String,
        override val sourceId: Long?,
    ) : PropertyChange
}
