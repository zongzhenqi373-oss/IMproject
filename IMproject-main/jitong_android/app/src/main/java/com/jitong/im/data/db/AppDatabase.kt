package com.jitong.im.data.db

import android.content.Context
import androidx.room.Database
import androidx.room.Room
import androidx.room.RoomDatabase
import net.zetetic.database.sqlcipher.SupportOpenHelperFactory

@Database(
    entities = [MessageEntity::class, MessageFtsEntity::class, ConversationEntity::class],
    version = 4,
    exportSchema = false,
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun messageDao(): MessageDao
    abstract fun conversationDao(): ConversationDao

    companion object {
        @Volatile
        private var instance: AppDatabase? = null

        @Volatile
        private var instanceOwnerId: Int? = null

        val MIGRATION_2_3 = object : androidx.room.migration.Migration(2, 3) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN fileId TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN fileName TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN fileSize INTEGER NOT NULL DEFAULT 0")
                db.execSQL("ALTER TABLE messages ADD COLUMN localPath TEXT")
                db.execSQL("ALTER TABLE messages ADD COLUMN transferred INTEGER NOT NULL DEFAULT 0")
            }
        }

        val MIGRATION_3_4 = object : androidx.room.migration.Migration(3, 4) {
            override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
                db.execSQL("ALTER TABLE messages ADD COLUMN contentType TEXT NOT NULL DEFAULT ''")
                db.execSQL("ALTER TABLE messages ADD COLUMN sha256 TEXT NOT NULL DEFAULT ''")
            }
        }

        /**
         * 打开指定账号的加密本地库；key 由调用方通过 DbKeyManager 拿到（登录后才可用）。
         * 每个 ownerId 各自一个物理库文件（jitong_<ownerId>.db），互不共享密钥，
         * 同一设备换号登录时不会因为密钥解不开而误清空另一个账号的本地缓存。
         */
        fun get(context: Context, ownerId: Int, key: ByteArray): AppDatabase = synchronized(this) {
            val cached = instance
            if (cached != null && instanceOwnerId == ownerId) return cached

            cached?.close() // 换了账号：关掉上一个账号的库连接，再开新账号的
            // 新版 sqlcipher-android 不再提供旧版 SQLiteDatabase.loadLibs(context)，
            // 需要在首次使用前显式装载 AAR 中的 native SQLCipher 库。
            System.loadLibrary("sqlcipher")
            val db = Room.databaseBuilder(
                context.applicationContext, AppDatabase::class.java, "jitong_$ownerId.db",
            )
                // Factory 会持有/使用传入的口令数组；传副本，避免影响调用方持有的 key。
                .openHelperFactory(SupportOpenHelperFactory(key.copyOf()))
                .addMigrations(MIGRATION_2_3, MIGRATION_3_4)
                // 演示项目：非预期升级路径仍直接重建本地库（消息可从服务端漫游/补发恢复）
                .fallbackToDestructiveMigration()
                .build()
            instance = db
            instanceOwnerId = ownerId
            db
        }

        /** 退出登录或 ViewModel 销毁时释放文件句柄，并清除单例引用。 */
        fun closeCurrent() = synchronized(this) {
            instance?.close()
            instance = null
            instanceOwnerId = null
        }
    }
}
