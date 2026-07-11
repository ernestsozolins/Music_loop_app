package com.audio.loopstation.data

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

@Database(
    entities = [SessionEntity::class, TrackEntity::class],
    version = 1,
    exportSchema = true,  // schema history belongs in version control
)
abstract class LoopStationDatabase : RoomDatabase() {

    abstract fun sessionDao(): SessionDao

    companion object {
        @Volatile private var instance: LoopStationDatabase? = null

        fun get(context: Context): LoopStationDatabase =
            instance ?: synchronized(this) {
                instance ?: Room.databaseBuilder(
                    context.applicationContext,
                    LoopStationDatabase::class.java,
                    "loopstation.db",
                ).build().also { instance = it }
            }
    }
}
