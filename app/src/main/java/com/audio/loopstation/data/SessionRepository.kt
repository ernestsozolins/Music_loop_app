package com.audio.loopstation.data

import kotlinx.coroutines.flow.Flow

/**
 * The persistence facade the ViewModel talks to. Wraps the DAO so the UI
 * layer never sees Room types beyond the entities, and owns the two-step
 * save protocol: the session row is created FIRST (so its id can name the
 * stem directory on disk), the engine then writes the stems, and finally
 * the metadata + track list are committed in one transaction.
 */
class SessionRepository(database: LoopStationDatabase) {

    private val dao = database.sessionDao()

    /** Everything the engine + UI need to persist for one track. */
    data class TrackSnapshot(
        val trackIndex: Int,
        val filePath: String,
        val volume: Float,
        val pan: Float,
        val isMuted: Boolean,
        val isSoloed: Boolean,
    )

    // ----- Flow-based observation (Compose recomposes on every change) -----

    fun observeSessions(): Flow<List<SessionEntity>> = dao.observeSessions()

    fun observeLatestSessionWithTracks(): Flow<SessionWithTracks?> =
        dao.observeLatestSessionWithTracks()

    fun observeSessionWithTracks(id: Long): Flow<SessionWithTracks?> =
        dao.observeSessionWithTracks(id)

    // ----- Startup restore -----

    /** The "last active" session = the most recently saved one. */
    suspend fun latestSessionWithTracks(): SessionWithTracks? = dao.latestSessionWithTracks()

    /** A specific session with its tracks (session browser "load"). */
    suspend fun sessionWithTracks(id: Long): SessionWithTracks? = dao.sessionWithTracks(id)

    // ----- Save protocol -----

    /**
     * Step 1: make sure the session row exists and return its id — the
     * caller uses it to build the stem directory before any file exists.
     */
    suspend fun ensureSessionId(existingId: Long?, name: String, bpm: Int, timeSignature: Int): Long {
        if (existingId != null && dao.sessionById(existingId) != null) return existingId
        return dao.insertSession(
            SessionEntity(
                name = name,
                timestamp = System.currentTimeMillis(),
                bpm = bpm,
                timeSignature = timeSignature,
            ),
        )
    }

    /**
     * Step 2 (after the stems are on disk): commit metadata and the full
     * track list atomically. Bumps [SessionEntity.timestamp], which is what
     * makes this the "last active" session for the next launch.
     */
    suspend fun commitSessionState(
        sessionId: Long,
        name: String?,
        bpm: Int,
        timeSignature: Int,
        tracks: List<TrackSnapshot>,
    ) {
        val existing = dao.sessionById(sessionId) ?: return
        val session = existing.copy(
            name = name ?: existing.name,
            timestamp = System.currentTimeMillis(),
            bpm = bpm,
            timeSignature = timeSignature,
        )
        dao.replaceSessionState(
            session,
            tracks.map {
                TrackEntity(
                    sessionId = sessionId,
                    trackIndex = it.trackIndex,
                    filePath = it.filePath,
                    volume = it.volume,
                    pan = it.pan,
                    isMuted = it.isMuted,
                    isSoloed = it.isSoloed,
                )
            },
        )
    }

    /** Removes the DB rows; the caller owns deleting the stem files. */
    suspend fun deleteSession(session: SessionEntity) = dao.deleteSession(session)
}
