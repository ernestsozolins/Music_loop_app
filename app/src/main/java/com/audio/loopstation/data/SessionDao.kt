package com.audio.loopstation.data

import androidx.room.Dao
import androidx.room.Delete
import androidx.room.Insert
import androidx.room.Query
import androidx.room.Transaction
import androidx.room.Update
import kotlinx.coroutines.flow.Flow

/**
 * Flow-returning queries re-emit whenever the underlying tables change, so
 * a Compose screen collecting them recomposes automatically on every save —
 * no manual refresh plumbing.
 */
@Dao
interface SessionDao {

    // ----- Observation (drives the UI) -----

    @Query("SELECT * FROM sessions ORDER BY timestamp DESC")
    fun observeSessions(): Flow<List<SessionEntity>>

    @Transaction
    @Query("SELECT * FROM sessions ORDER BY timestamp DESC LIMIT 1")
    fun observeLatestSessionWithTracks(): Flow<SessionWithTracks?>

    @Transaction
    @Query("SELECT * FROM sessions WHERE id = :id")
    fun observeSessionWithTracks(id: Long): Flow<SessionWithTracks?>

    // ----- One-shot reads (startup restore) -----

    @Transaction
    @Query("SELECT * FROM sessions ORDER BY timestamp DESC LIMIT 1")
    suspend fun latestSessionWithTracks(): SessionWithTracks?

    @Transaction
    @Query("SELECT * FROM sessions WHERE id = :id")
    suspend fun sessionWithTracks(id: Long): SessionWithTracks?

    @Query("SELECT * FROM sessions WHERE id = :id")
    suspend fun sessionById(id: Long): SessionEntity?

    // ----- Writes -----

    @Insert
    suspend fun insertSession(session: SessionEntity): Long

    @Update
    suspend fun updateSession(session: SessionEntity)

    @Insert
    suspend fun insertTracks(tracks: List<TrackEntity>)

    @Query("DELETE FROM tracks WHERE sessionId = :sessionId")
    suspend fun deleteTracksForSession(sessionId: Long)

    @Delete
    suspend fun deleteSession(session: SessionEntity)  // tracks CASCADE

    /** Atomically replaces a session's metadata and its full track list. */
    @Transaction
    suspend fun replaceSessionState(session: SessionEntity, tracks: List<TrackEntity>) {
        updateSession(session)
        deleteTracksForSession(session.id)
        if (tracks.isNotEmpty()) insertTracks(tracks)
    }
}
