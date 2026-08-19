package com.jitong.im.data.db

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase

@Database(
    entities = [MessageEntity::class, MessageFtsEntity::class, ConversationEntity::class],
    version = 2, // v2：messages 表新增 seq 列（会话级序列号）
    exportSchema = false,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun messageDao(): MessageDao
    abstract fun conversationDao(): ConversationDao

    companion object {
        @Volatile
        private var instance: AppDatabase? = null

        fun get(context: Context): AppDatabase =
            instance ?: synchronized(this) {
                instance ?: Room.databaseBuilder(
                    context.applicationContext, AppDatabase::class.java, "jitong.db",
                )
                    // 演示项目：schema 升级直接重建本地库（消息可从服务端漫游/补发恢复）
                    .fallbackToDestructiveMigration()
                    .build().also { instance = it }
            }
    }
}
