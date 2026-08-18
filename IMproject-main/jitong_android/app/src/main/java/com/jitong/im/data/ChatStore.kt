package com.jitong.im.data

import android.content.Context
import com.jitong.im.data.db.AppDatabase
import com.jitong.im.data.db.ConversationEntity
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.ui.ChatMessage
import com.jitong.im.ui.MsgKind
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 消息仓储：ViewModel 内存态 ↔ Room 持久态的桥。
 * 图片字节写应用私有目录 filesDir/img/，库里只存路径（media_path 口径与服务端一致）。
 */
class ChatStore(context: Context) {

    private val appContext = context.applicationContext
    private val db = AppDatabase.get(appContext)

    /** 会话 id：min(id)*K + max(id)，双向同值，与服务端算法一致（K=1<<20） */
    fun conversationId(a: Int, b: Int): Long {
        val lo = minOf(a, b).toLong()
        val hi = maxOf(a, b).toLong()
        return lo * CONV_K + hi
    }

    /** 消息落库（含 FTS 同事务双写 + 会话行刷新）。重复 msg_id 幂等忽略。 */
    suspend fun save(ownerId: Int, m: ChatMessage, incrUnread: Boolean) = withContext(Dispatchers.IO) {
        var mediaPath: String? = null
        if (m.kind == MsgKind.IMAGE && m.imageBytes != null) {
            mediaPath = writeImageFile(m.msgId, m.imageBytes)
        }
        val inserted = db.messageDao().insertWithFts(m.toEntity(ownerId, mediaPath))
        if (inserted) {
            val lastMsg = if (m.kind == MsgKind.IMAGE) "[图片]" else m.text
            db.conversationDao().upsertOnMessage(
                conversationId(ownerId, m.peerId), ownerId, m.peerId,
                lastMsg, m.ts, incrUnread && !m.fromMe,
            )
        }
    }

    suspend fun updateStatus(ownerId: Int, msgId: String, status: ChatMessage.Status) =
        withContext(Dispatchers.IO) {
            db.messageDao().updateStatus(ownerId, msgId, status.toDb())
        }

    suspend fun clearUnread(ownerId: Int, peerId: Int) = withContext(Dispatchers.IO) {
        db.conversationDao().clearUnread(conversationId(ownerId, peerId))
    }

    /** 登录后装载：历史消息（含图片字节读回）+ 会话行 */
    suspend fun hydrate(ownerId: Int): Pair<Map<Int, List<ChatMessage>>, Map<Int, ConversationEntity>> =
        withContext(Dispatchers.IO) {
            val messages = db.messageDao().allForOwner(ownerId)
                .map { it.toChatMessage() }
                .groupBy { it.peerId }
            val convs = db.conversationDao().allForOwner(ownerId).associateBy { it.peerId }
            messages to convs
        }

    /** FTS 前缀 + LIKE 子串双路搜索，按 msgId 去重、时间倒序 */
    suspend fun search(ownerId: Int, kw: String): List<MessageEntity> = withContext(Dispatchers.IO) {
        val pattern = "\"" + kw.replace("\"", "\"\"") + "\"*"
        val fts = runCatching { db.messageDao().searchFts(ownerId, pattern) }.getOrDefault(emptyList())
        val like = db.messageDao().searchLike(ownerId, kw)
        (fts + like).distinctBy { it.msgId }.sortedByDescending { it.ts }
    }

    // ---------------- 内部 ----------------

    private fun writeImageFile(msgId: String, bytes: ByteArray): String {
        val dir = File(appContext.filesDir, "img").apply { mkdirs() }
        val file = File(dir, msgId + extOf(bytes))
        if (!file.exists()) file.writeBytes(bytes) // msg_id 幂等
        return file.absolutePath
    }

    private fun extOf(bytes: ByteArray): String = when {
        bytes.size >= 8 && bytes[0] == 0x89.toByte() && bytes[1] == 0x50.toByte() -> ".png"
        bytes.size >= 3 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xD8.toByte() -> ".jpg"
        else -> ".bin"
    }

    private fun MessageEntity.toChatMessage(): ChatMessage {
        val bytes = mediaPath?.let { p -> runCatching { File(p).readBytes() }.getOrNull() }
        return ChatMessage(
            msgId = msgId, peerId = peerId, fromMe = fromMe,
            text = content.orEmpty(),
            kind = if (type == 1) MsgKind.IMAGE else MsgKind.TEXT,
            imageBytes = bytes, imgW = imgW, imgH = imgH, ts = ts,
            status = when (status) {
                0 -> ChatMessage.Status.SENDING
                1 -> ChatMessage.Status.DELIVERED
                3 -> ChatMessage.Status.OFFLINE_STORED
                else -> ChatMessage.Status.RECEIVED
            },
        )
    }

    private fun ChatMessage.toEntity(ownerId: Int, mediaPath: String?) = MessageEntity(
        ownerId = ownerId, msgId = msgId,
        conversationId = conversationId(ownerId, peerId),
        peerId = peerId, fromMe = fromMe,
        type = if (kind == MsgKind.IMAGE) 1 else 0,
        content = if (kind == MsgKind.TEXT) text else null,
        mediaPath = mediaPath,
        imgW = imgW, imgH = imgH, ts = ts,
        status = status.toDb(),
    )

    private fun ChatMessage.Status.toDb() = when (this) {
        ChatMessage.Status.SENDING -> 0
        ChatMessage.Status.DELIVERED -> 1
        ChatMessage.Status.RECEIVED -> 2
        ChatMessage.Status.OFFLINE_STORED -> 3
    }

    companion object {
        private const val CONV_K = 1L shl 20
    }
}
