package com.example.gnssble.data.db

import androidx.room.Dao
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

@Dao
interface GnssDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertSession(session: SessionEntity): Long

    @Query("UPDATE sessions SET endedAt = :endedAt WHERE id = :sessionId")
    suspend fun endSession(sessionId: Long, endedAt: Long)

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertTrackPoint(point: TrackPointEntity)

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun insertDebugEvent(event: DebugEventEntity)

    @Query("SELECT * FROM sessions ORDER BY startedAt DESC")
    fun observeSessions(): Flow<List<SessionEntity>>

    @Query("SELECT * FROM track_points WHERE sessionId = :sessionId ORDER BY tsMs ASC")
    suspend fun getTrackPoints(sessionId: Long): List<TrackPointEntity>

    @Query("SELECT * FROM debug_events WHERE sessionId = :sessionId ORDER BY tsMs ASC")
    suspend fun getDebugEvents(sessionId: Long): List<DebugEventEntity>
}
