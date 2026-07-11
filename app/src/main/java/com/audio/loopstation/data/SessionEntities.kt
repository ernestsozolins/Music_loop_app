package com.audio.loopstation.data

import androidx.room.Embedded
import androidx.room.Entity
import androidx.room.ForeignKey
import androidx.room.Index
import androidx.room.PrimaryKey
import androidx.room.Relation

/** One saved looper session. */
@Entity(tableName = "sessions")
data class SessionEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val name: String,
    /** Last-saved wall-clock time (epoch millis); newest = "last active". */
    val timestamp: Long,
    val bpm: Int,
    /** Beats per measure for the metronome (e.g. 4 for 4/4, 3 for 3/4). */
    val timeSignature: Int,
)

/**
 * One saved loop track. [filePath] points at the float32 stem .wav the
 * engine exported for this track; the mix parameters mirror the UI state so
 * a reopened session sounds exactly like it did when it was closed.
 */
@Entity(
    tableName = "tracks",
    foreignKeys = [
        ForeignKey(
            entity = SessionEntity::class,
            parentColumns = ["id"],
            childColumns = ["sessionId"],
            onDelete = ForeignKey.CASCADE,
        ),
    ],
    indices = [Index("sessionId")],
)
data class TrackEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val sessionId: Long,
    /** Engine track slot (0-based) this stem is loaded back into. */
    val trackIndex: Int,
    val filePath: String,
    val volume: Float,   // 0..1
    val pan: Float,      // -1..1
    val isMuted: Boolean,
    val isSoloed: Boolean,
)

/** A session joined with its tracks (Room fills [tracks] via the relation). */
data class SessionWithTracks(
    @Embedded val session: SessionEntity,
    @Relation(parentColumn = "id", entityColumn = "sessionId")
    val tracks: List<TrackEntity>,
)
