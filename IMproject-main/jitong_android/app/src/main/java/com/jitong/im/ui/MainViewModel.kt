package com.jitong.im.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.jitong.im.data.ChatStore
import com.jitong.im.data.Prefs
import com.jitong.im.data.db.ConversationEntity
import com.jitong.im.data.db.MessageEntity
import com.jitong.im.net.ImClient
import com.jitong.im.net.Protocol
import com.jitong.im.net.sha256Hex
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.util.UUID

enum class Screen { Login, FriendList, Chat }

data class Friend(
    val id: Int,
    val nick: String,
    val feeling: String,
    val online: Boolean,
)

enum class MsgKind { TEXT, IMAGE, FILE }

data class ChatMessage(
    val msgId: String,
    val peerId: Int,
    val fromMe: Boolean,
    val text: String = "",
    val kind: MsgKind = MsgKind.TEXT,
    val imageBytes: ByteArray? = null,
    val imgW: Int = 0,
    val imgH: Int = 0,
    val ts: Long = System.currentTimeMillis(),
    val seq: Long = 0,      // 会话级序列号（服务端分配）；本端刚发出、未确认的消息为 0，排在末尾
    val fileId: String = "",
    val fileName: String = "",
    val fileSize: Long = 0,
    val localPath: String? = null,
    val transferred: Int = 0,
    val status: Status = Status.RECEIVED,
) {
    enum class Status { SENDING, DELIVERED, OFFLINE_STORED, RECEIVED }
}

class MainViewModel : ViewModel() {

    private val client = ImClient()
    private var store: ChatStore? = null

    /** 当前登录账号 id（未登录为 0） */
    val myId: Int get() = client.myId

    private val _screen = MutableStateFlow(Screen.Login)
    val screen: StateFlow<Screen> = _screen

    private val _loginTip = MutableStateFlow("")
    val loginTip: StateFlow<String> = _loginTip

    private val _myNick = MutableStateFlow("")
    val myNick: StateFlow<String> = _myNick

    private val _myFeeling = MutableStateFlow("")
    val myFeeling: StateFlow<String> = _myFeeling

    /** 好友列表：在线的排前面 */
    private val _friends = MutableStateFlow<List<Friend>>(emptyList())
    val friends: StateFlow<List<Friend>> = _friends

    /** 会话消息：peerId -> 有序消息列表（登录后从 Room 装载，运行期内存驻留） */
    private val _messages = MutableStateFlow<Map<Int, List<ChatMessage>>>(emptyMap())
    val messages: StateFlow<Map<Int, List<ChatMessage>>> = _messages

    /** 会话行：peerId -> 最后消息/未读数（好友列表行展示用） */
    private val _conversations = MutableStateFlow<Map<Int, ConversationEntity>>(emptyMap())
    val conversations: StateFlow<Map<Int, ConversationEntity>> = _conversations

    private val _chatPeer = MutableStateFlow<Friend?>(null)
    val chatPeer: StateFlow<Friend?> = _chatPeer

    /** 搜索结果（聊天记录） */
    private val _searchResults = MutableStateFlow<List<MessageEntity>>(emptyList())
    val searchResults: StateFlow<List<MessageEntity>> = _searchResults

    private val _toast = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val toast: SharedFlow<String> = _toast

    private var expectDisconnect = false
    private var lastTel: String? = null
    private var lastHash: String? = null
    private var pendingRemember = false // 本次登录是否勾选“记住账号密码”
    private var pendingPass: String? = null // 本次登录的明文密码（记住时持久化用于回填）

    /** 断线自动重连任务与状态（防止并发重连、登录成功后清零退避） */
    private var reconnectJob: kotlinx.coroutines.Job? = null
    private var reconnecting = false

    /** 漫游分页状态：每会话已加载最小 seq（上拉游标）、是否还有更早、是否正在加载（防抖） */
    private val loadedMinSeq = mutableMapOf<Int, Long>()
    private val roamHasMore = mutableMapOf<Int, Boolean>()
    private val roamLoading = mutableSetOf<Int>()

    init {
        viewModelScope.launch { collectEvents() }
    }

    /** MainActivity 注入仓储：仅注入本地库，不做自动登录（登录页会回填记住的账号密码，由用户手动登录） */
    fun attachStore(store: ChatStore) {
        if (this.store != null) return
        this.store = store
    }

    // ---------------- 登录 / 注册 ----------------

    fun login(tel: String, pass: String, remember: Boolean) {
        if (tel.isBlank() || pass.isBlank()) {
            _loginTip.value = "请输入手机号和密码"
            return
        }
        lastTel = tel.trim()
        lastHash = sha256Hex(pass) // 供断线自动重连使用
        pendingRemember = remember // 登录成功后据此决定是否持久化账号密码
        pendingPass = pass         // 记住时保存的明文密码（用于登录页回填）
        viewModelScope.launch {
            _loginTip.value = "连接服务器…"
            if (!client.connect(ImClient.DEFAULT_HOST)) {
                _loginTip.value = "连接失败，请确认 im_server 已启动"
                return@launch
            }
            _loginTip.value = "登录中…"
            client.login(tel.trim(), pass)
        }
    }

    fun register(nick: String, tel: String, pass: String) {
        if (nick.isBlank() || tel.isBlank() || pass.isBlank()) {
            _loginTip.value = "请填写完整注册信息"
            return
        }
        viewModelScope.launch {
            _loginTip.value = "连接服务器…"
            if (!client.connect(ImClient.DEFAULT_HOST)) {
                _loginTip.value = "连接失败，请确认 im_server 已启动"
                return@launch
            }
            _loginTip.value = "注册中…"
            client.register(nick.trim(), tel.trim(), pass)
        }
    }

    // ---------------- 导航 ----------------

    fun openChat(friend: Friend) {
        _chatPeer.value = friend
        _screen.value = Screen.Chat
        // 进入会话清零未读（库 + 内存）
        viewModelScope.launch {
            store?.clearUnread(client.myId, friend.id)
            _conversations.value[friend.id]?.let {
                _conversations.value = _conversations.value + (friend.id to it.copy(unread = 0))
            }
        }
        // 漫游游标初始化：本地无历史则拉最近一页；本地已有则以本地最小 seq 为上拉起点，允许继续向上翻更早的
        val local = _messages.value[friend.id].orEmpty()
        if (local.isEmpty()) {
            requestHistory(friend.id, Long.MAX_VALUE)
        } else if (loadedMinSeq[friend.id] == null) {
            val minSeq = local.filter { it.seq > 0 }.minOfOrNull { it.seq }
            if (minSeq != null) {
                loadedMinSeq[friend.id] = minSeq
                roamHasMore[friend.id] = true // 未知，允许尝试上拉；服务端返回空批会置 false
            }
        }
    }

    /** 上拉加载更早历史：hasMore 且未在加载时才发请求（防抖，修复 6） */
    fun loadMoreHistory(peerId: Int) {
        if (roamHasMore[peerId] == false) return       // 已到头
        if (peerId in roamLoading) return               // 正在加载，避免并发游标错乱
        val cursor = loadedMinSeq[peerId] ?: return     // 尚无首页则不上拉（首页由 openChat 触发）
        requestHistory(peerId, cursor)
    }

    /** 发起一页历史漫游请求（首页 beforeSeq=Long.MAX，上拉传当前最小 seq） */
    private fun requestHistory(peerId: Int, beforeSeq: Long) {
        if (peerId in roamLoading) return
        roamLoading.add(peerId)
        viewModelScope.launch { client.roamMessages(peerId, beforeSeq, PAGE_SIZE) }
    }

    fun backToFriends() {
        _screen.value = Screen.FriendList
    }

    fun logout() {
        expectDisconnect = true
        Prefs.clearCredentialsIfNotRemember()
        viewModelScope.launch { client.logout() }
        resetToLogin()
    }

    // ---------------- 聊天 ----------------

    fun send(text: String) {
        val peer = _chatPeer.value ?: return
        if (text.isBlank()) return
        val msgId = UUID.randomUUID().toString()
        android.util.Log.d("IMSEQ", "发送消息 to=${peer.id} msgId=$msgId seq(待服务端分配)=0 text=$text")
        val m = ChatMessage(msgId, peer.id, fromMe = true, text = text, status = ChatMessage.Status.SENDING)
        append(m, incrUnread = false)
        viewModelScope.launch { client.sendChat(peer.id, text, msgId) }
    }

    /** 发送图片（压缩已由调用方完成），本地即时上屏 + 等待回执 */
    fun sendImage(bytes: ByteArray, w: Int, h: Int) {
        val peer = _chatPeer.value ?: return
        if (bytes.isEmpty()) return
        val msgId = UUID.randomUUID().toString()
        append(
            ChatMessage(
                msgId, peer.id, fromMe = true, kind = MsgKind.IMAGE,
                imageBytes = bytes, imgW = w, imgH = h,
                status = ChatMessage.Status.SENDING,
            ),
            incrUnread = false,
        )
        viewModelScope.launch { client.sendImage(peer.id, bytes, w, h, msgId) }
    }

    /** 聊天记录搜索（FTS 前缀 + LIKE 子串） */
    fun search(kw: String) {
        if (kw.isBlank()) {
            _searchResults.value = emptyList()
            return
        }
        val s = store ?: return
        viewModelScope.launch {
            _searchResults.value = s.search(client.myId, kw.trim())
        }
    }

    fun notify(msg: String) {
        viewModelScope.launch { _toast.emit(msg) }
    }

    // ---------------- 文件发送（发送方状态机 + 断点续传） ----------------

    /** 进行中的上传：fileId -> 待发字节源信息 */
    private data class Upload(val uri: android.net.Uri, val peerId: Int, val name: String, val size: Long, val totalChunks: Int)
    private val uploads = mutableMapOf<String, Upload>()

    @Volatile private var appResolver: android.content.ContentResolver? = null
    fun attachResolver(r: android.content.ContentResolver) { appResolver = r }

    @Volatile private var appContext: android.content.Context? = null
    fun attachContext(ctx: android.content.Context) { appContext = ctx.applicationContext }

    fun sendFile(uri: android.net.Uri, resolver: android.content.ContentResolver) {
        val peer = _chatPeer.value ?: return
        val (name, size) = queryNameSize(resolver, uri) ?: return
        if (size > Protocol.FILE_MAX_SIZE) { notify("文件超过 100MB"); return }
        val msgId = java.util.UUID.randomUUID().toString()
        val totalChunks = ((size + Protocol.FILE_CHUNK_SIZE - 1) / Protocol.FILE_CHUNK_SIZE).toInt()
        uploads[msgId] = Upload(uri, peer.id, name, size, totalChunks)
        append(ChatMessage(msgId, peer.id, fromMe = true, kind = MsgKind.FILE,
            fileId = msgId, fileName = name, fileSize = size, localPath = uri.toString(),
            status = ChatMessage.Status.SENDING), incrUnread = false)
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val sha = resolver.openInputStream(uri)!!.use { com.jitong.im.net.sha256HexOfStream(it) }
            client.fileOffer(msgId, peer.id, name, size, totalChunks, sha)
        }
    }

    private fun queryNameSize(resolver: android.content.ContentResolver, uri: android.net.Uri): Pair<String, Long>? {
        resolver.query(uri, null, null, null, null)?.use { c ->
            val ni = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            val si = c.getColumnIndex(android.provider.OpenableColumns.SIZE)
            if (c.moveToFirst()) {
                val name = if (ni >= 0) c.getString(ni) else "file"
                val size = if (si >= 0) c.getLong(si) else -1L
                if (size >= 0) return name to size
            }
        }
        return null
    }

    // ---------------- 文件接收（接收方状态机 + 断点续传） ----------------

    /** 进行中的下载：fileId -> 落盘中的 .part 文件信息 */
    private data class Download(val peerId: Int, val fileId: String, val name: String, val size: Long,
                                val out: java.io.OutputStream, val partFile: java.io.File)
    private val downloads = mutableMapOf<String, Download>()

    /** 用户点击下载：从本地已有的 .part 大小推算续传起点，向服务端请求剩余分片 */
    fun downloadFile(msg: ChatMessage) {
        val ctx = appContext ?: return
        val dir = java.io.File(ctx.filesDir, "file").apply { mkdirs() }
        val part = java.io.File(dir, "${msg.fileId}_${msg.fileName}.part")
        val fromChunk = (part.length() / Protocol.FILE_CHUNK_SIZE).toInt()
        val out = java.io.FileOutputStream(part, /*append=*/true)
        downloads[msg.fileId] = Download(msg.peerId, msg.fileId, msg.fileName, msg.fileSize, out, part)
        updateFileStatus(msg.peerId, msg.msgId, ChatMessage.Status.SENDING) // 复用"进行中"
        viewModelScope.launch { client.fileDownload(msg.fileId, fromChunk) }
    }

    // ---------------- 事件归集 ----------------

    private suspend fun collectEvents() {
        client.events.collect { e ->
            when (e) {
                is ImClient.Event.RegisterResult -> {
                    _loginTip.value = when (e.result) {
                        Protocol.REGISTER_SUCC -> "注册成功，请登录"
                        Protocol.REGISTER_NICK_EXIT -> "注册失败：昵称已存在"
                        else -> "注册失败：手机号已存在"
                    }
                }

                is ImClient.Event.LoginResult -> when (e.result) {
                    Protocol.LOGIN_SUCCESS -> {
                        _loginTip.value = ""
                        // 登录成功：结束任何进行中的重连、清零退避状态
                        reconnecting = false
                        reconnectJob?.cancel()
                        reconnectJob = null
                        // 记住账号密码：勾选则持久化明文账号密码（仅用于登录页回填，不自动登录）；
                        // 未勾选则清除。记住开关本身也持久化。
                        Prefs.remember = pendingRemember
                        if (pendingRemember) {
                            lastTel?.let { Prefs.tel = it }
                            pendingPass?.let { Prefs.pass = it }
                        } else {
                            Prefs.tel = null
                            Prefs.pass = null
                        }
                        _screen.value = Screen.FriendList
                        store?.let { s ->
                            viewModelScope.launch {
                                val (msgs, convs) = s.hydrate(client.myId)
                                android.util.Log.d("IMDBG", "hydrate done dbMsgs=${msgs.mapValues { it.value.size }} memMsgs=${_messages.value.mapValues { it.value.size }}")
                                // 合并而非覆盖：hydrate 是异步 IO，其间登录补发的离线消息
                                // 可能已经通过 append 进入内存，直接整体赋值会把这些新消息冲掉，
                                // 导致“发送方回执已翻转、接收方却看不到补发消息”。
                                // 以 DB 结果为基底，叠加当前内存里已有的消息（按 msgId 去重）。
                                _messages.value = mergeMessages(msgs, _messages.value)
                                _conversations.value = mergeConversations(convs, _conversations.value)
                                android.util.Log.d("IMDBG", "after merge msgs=${_messages.value.mapValues { it.value.size }}")
                            }
                        }
                        // 消息漫游：登录后拉每会话最后一条恢复会话列表预览（换设备/重装后本地库空时尤为关键）
                        viewModelScope.launch { client.roamConversations() }
                    }
                    Protocol.LOGIN_NOTEXIT -> _loginTip.value = "登录失败：用户不存在"
                    else -> _loginTip.value = "登录失败：密码错误"
                }

                is ImClient.Event.UserOrFriendInfo -> {
                    if (e.userId == client.myId) {
                        _myNick.value = e.nick
                        _myFeeling.value = e.feeling
                    } else {
                        upsertFriend(Friend(e.userId, e.nick, e.feeling, e.online))
                        _chatPeer.value?.takeIf { it.id == e.userId }?.let {
                            _chatPeer.value = it.copy(online = e.online)
                        }
                    }
                }

                is ImClient.Event.ChatReceived -> {
                    val inChat = _screen.value == Screen.Chat && _chatPeer.value?.id == e.fromId
                    android.util.Log.d("IMDBG", "ChatReceived from=${e.fromId} msgId=${e.msgId} ts=${e.ts} seq=${e.seq} text=${e.text}")
                    append(
                        ChatMessage(e.msgId, e.fromId, fromMe = false, text = e.text, ts = e.ts, seq = e.seq),
                        incrUnread = !inChat,
                    )
                }

                is ImClient.Event.ImageReceived -> {
                    val inChat = _screen.value == Screen.Chat && _chatPeer.value?.id == e.fromId
                    append(
                        ChatMessage(
                            e.msgId, e.fromId, fromMe = false, kind = MsgKind.IMAGE,
                            imageBytes = e.bytes, imgW = e.w, imgH = e.h, ts = e.ts, seq = e.seq,
                        ),
                        incrUnread = !inChat,
                    )
                }

                is ImClient.Event.ChatSendResult -> {
                    android.util.Log.d("IMSEQ", "收到回执 peer=${e.peerId} msgId=${e.msgId} 服务端分配 seq=${e.seq} result=${e.result}")
                    updateStatus(
                        e.peerId, e.msgId,
                        if (e.result == Protocol.CHAT_RESULT_SUCC)
                            ChatMessage.Status.DELIVERED else ChatMessage.Status.OFFLINE_STORED,
                        e.seq,
                    )
                }

                is ImClient.Event.RoamConversations -> {
                    // 会话列表末条：只更新会话预览行（不落消息表，避免"半条无字节图片"抢占 msgId 唯一约束，修复 3）
                    for (item in e.convs) {
                        val peerId = if (item.fromId == client.myId) item.toId else item.fromId
                        val preview = if (item.isImage) "[图片]" else item.text
                        val old = _conversations.value[peerId]
                        // mergeConversations 语义：仅当更新（ts 更晚）才覆盖，避免冲掉刚到达的更新消息（修复 5）
                        if (old == null || item.ts >= old.lastTs) {
                            _conversations.value = _conversations.value + (
                                peerId to com.jitong.im.data.db.ConversationEntity(
                                    conversationId = 0L, ownerId = client.myId, peerId = peerId,
                                    lastMsg = preview, lastTs = item.ts, unread = old?.unread ?: 0,
                                )
                                )
                        }
                    }
                }

                is ImClient.Event.RoamMessages -> {
                    // 历史分页落地：每条走 append（msgId 幂等 + sortBySeq），但不刷新会话预览（旧消息）
                    for (item in e.msgs) {
                        val peerId = if (item.fromId == client.myId) item.toId else item.fromId
                        val fromMe = item.fromId == client.myId
                        append(
                            ChatMessage(
                                msgId = item.msgId, peerId = peerId, fromMe = fromMe,
                                text = item.text,
                                kind = if (item.isImage) MsgKind.IMAGE else MsgKind.TEXT,
                                imageBytes = item.bytes, imgW = item.w, imgH = item.h,
                                ts = item.ts, seq = item.seq,
                                status = if (fromMe) ChatMessage.Status.DELIVERED else ChatMessage.Status.RECEIVED,
                            ),
                            incrUnread = false,
                            updateConversation = false,
                        )
                    }
                    // 更新分页游标：minSeq 为本批最小 seq，供下次上拉；hasMore 决定是否继续
                    if (e.minSeq > 0) {
                        val cur = loadedMinSeq[e.peerId]
                        if (cur == null || e.minSeq < cur) loadedMinSeq[e.peerId] = e.minSeq
                    }
                    roamHasMore[e.peerId] = e.hasMore
                    roamLoading.remove(e.peerId)
                }

                is ImClient.Event.FriendOffline -> {
                    _friends.value = _friends.value.map {
                        if (it.id == e.userId) it.copy(online = false) else it
                    }
                    _chatPeer.value?.takeIf { it.id == e.userId }?.let {
                        _chatPeer.value = it.copy(online = false)
                    }
                }

                ImClient.Event.KickedOffline -> {
                    expectDisconnect = true
                    Prefs.clearCredentialsIfNotRemember()
                    client.disconnect()
                    _toast.emit("你的账号已在其他设备登录")
                    resetToLogin()
                }

                ImClient.Event.Disconnected -> {
                    if (!expectDisconnect && _screen.value != Screen.Login) {
                        // 非预期断开：不直接回登录页，先尝试自动重连（IM 标配，弱网/瞬断可自愈）
                        val tel = lastTel
                        val hash = lastHash
                        if (!tel.isNullOrEmpty() && !hash.isNullOrEmpty()) {
                            startReconnect(tel, hash)
                        } else {
                            _toast.emit("与服务器断开连接")
                            resetToLogin()
                        }
                    }
                    expectDisconnect = false
                }

                // 文件发送方状态机（M7 Task 11）：FileChunk/FileCard 为接收方逻辑，留给 Task 12
                is ImClient.Event.FileOfferResult -> {
                    val up = uploads[e.msgId] ?: return@collect
                    if (e.result != Protocol.FILE_OFFER_OK) {
                        updateFileStatus(up.peerId, e.msgId, ChatMessage.Status.OFFLINE_STORED) // 复用"失败"展示
                        notify("文件被拒绝（超限）"); uploads.remove(e.msgId); return@collect
                    }
                    val resolver = appResolver ?: return@collect
                    viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                        resolver.openInputStream(up.uri)!!.use { ins ->
                            var idx = 0
                            val buf = ByteArray(Protocol.FILE_CHUNK_SIZE)
                            // 跳过已发送的 e.receivedChunks 块（断点续传）
                            var skip = e.receivedChunks.toLong() * Protocol.FILE_CHUNK_SIZE
                            while (skip > 0) { val s = ins.skip(skip); if (s <= 0) break; skip -= s }
                            idx = e.receivedChunks
                            while (true) {
                                val n = ins.read(buf); if (n < 0) break
                                client.fileChunk(e.fileId, idx, buf.copyOf(n)); idx++
                            }
                            client.fileComplete(e.fileId, e.msgId)
                        }
                    }
                }
                is ImClient.Event.FileChunk -> {
                    val d = downloads[e.fileId] ?: return@collect
                    kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { d.out.write(e.data); d.out.flush() }
                    val chunks = (d.partFile.length() / Protocol.FILE_CHUNK_SIZE).toInt()
                    updateFileProgress(d.peerId, e.fileId, chunks)
                }
                is ImClient.Event.FileProgress -> {
                    val up = uploads[e.fileId]
                    if (up != null) {
                        if (e.status == Protocol.FILE_ST_DONE) {
                            updateFileStatus(up.peerId, e.fileId, ChatMessage.Status.DELIVERED)
                            uploads.remove(e.fileId)
                        } else if (e.status == Protocol.FILE_ST_FAILED) {
                            notify("文件发送失败"); uploads.remove(e.fileId)
                        } else {
                            updateFileProgress(up.peerId, e.fileId, e.received)
                        }
                    } else {
                        // 下载进度（接收方）
                        onDownloadProgress(e)
                    }
                }
                is ImClient.Event.FileCard -> {
                    val inChat = _screen.value == Screen.Chat && _chatPeer.value?.id == e.fromId
                    append(ChatMessage(e.msgId, e.fromId, fromMe = false, kind = MsgKind.FILE,
                        fileId = e.fileId, fileName = e.name, fileSize = e.size, ts = e.ts, seq = e.seq,
                        status = ChatMessage.Status.RECEIVED), incrUnread = !inChat)
                }
            }
        }
    }

    private fun upsertFriend(f: Friend) {
        val list = _friends.value.toMutableList()
        val idx = list.indexOfFirst { it.id == f.id }
        if (idx >= 0) list[idx] = f else list.add(f)
        _friends.value = list.sortedWith(compareBy({ !it.online }, { it.id }))
    }

    /** msg_id 去重（内存 + Room UNIQUE 双保险），并按需落库。
     *  updateConversation=false：漫游历史（旧消息）只入消息列表，不刷新会话预览行，避免回退 lastTs */
    private fun append(msg: ChatMessage, incrUnread: Boolean, updateConversation: Boolean = true) {
        val conv = _messages.value[msg.peerId].orEmpty()
        if (conv.any { it.msgId == msg.msgId }) return
        // 按会话 seq 插入并保持有序（补发/乱序到达时也能排到正确位置）
        _messages.value = _messages.value + (msg.peerId to sortBySeq(conv + msg))

        if (updateConversation) {
            // 会话行内存即时刷新
            val lastMsg = if (msg.kind == MsgKind.IMAGE) "[图片]" else msg.text
            val old = _conversations.value[msg.peerId]
            val unread = (old?.unread ?: 0) + if (incrUnread && !msg.fromMe) 1 else 0
            _conversations.value = _conversations.value + (
                msg.peerId to com.jitong.im.data.db.ConversationEntity(
                    conversationId = 0L, ownerId = client.myId, peerId = msg.peerId,
                    lastMsg = lastMsg, lastTs = msg.ts, unread = unread,
                )
                )
        }

        viewModelScope.launch { store?.save(client.myId, msg, incrUnread, bumpConversation = updateConversation) }
    }

    /**
     * 合并 hydrate 的 DB 结果与内存中已到达的消息（按 msgId 去重、按会话 seq 排序）。
     * 用于登录时避免异步 hydrate 覆盖掉期间补发到内存的离线消息。
     * 排序主键为服务端分配的会话级 seq（严格顺序）；本端刚发出、尚未确认的消息 seq=0，
     * 视为最新排在末尾（用 ts 兜底其相对顺序）。
     */
    private fun mergeMessages(
        fromDb: Map<Int, List<ChatMessage>>,
        inMemory: Map<Int, List<ChatMessage>>,
    ): Map<Int, List<ChatMessage>> {
        if (inMemory.isEmpty()) return fromDb
        val result = fromDb.toMutableMap()
        for ((peerId, memList) in inMemory) {
            val base = result[peerId].orEmpty()
            val existingIds = base.mapTo(HashSet()) { it.msgId }
            val extra = memList.filter { it.msgId !in existingIds }
            if (extra.isEmpty()) continue
            result[peerId] = sortBySeq(base + extra)
        }
        return result
    }

    /** 会话内消息排序：seq>0 按 seq 升序；seq=0（本端未确认）排末尾，用 ts 兜底 */
    private fun sortBySeq(list: List<ChatMessage>): List<ChatMessage> =
        list.sortedWith(compareBy({ if (it.seq > 0) it.seq else Long.MAX_VALUE }, { it.ts }))

    /** 会话行合并：DB 为基底，内存里更新的会话行（lastTs 更新）优先保留。 */
    private fun mergeConversations(
        fromDb: Map<Int, ConversationEntity>,
        inMemory: Map<Int, ConversationEntity>,
    ): Map<Int, ConversationEntity> {
        if (inMemory.isEmpty()) return fromDb
        val result = fromDb.toMutableMap()
        for ((peerId, memConv) in inMemory) {
            val dbConv = result[peerId]
            if (dbConv == null || memConv.lastTs >= dbConv.lastTs) {
                result[peerId] = memConv
            }
        }
        return result
    }

    private fun updateStatus(peerId: Int, msgId: String, status: ChatMessage.Status, seq: Long = 0) {
        val conv = _messages.value[peerId] ?: return
        // 回执携带服务端分配的 seq：更新状态同时校正本端消息的 seq，并按 seq 重排
        val updated = conv.map {
            if (it.msgId == msgId) it.copy(status = status, seq = if (seq > 0) seq else it.seq) else it
        }
        _messages.value = _messages.value + (peerId to if (seq > 0) sortBySeq(updated) else updated)
        viewModelScope.launch {
            store?.updateStatus(client.myId, msgId, status)
            if (seq > 0) store?.updateSeq(client.myId, msgId, seq)
        }
    }

    private fun updateFileStatus(peerId: Int, msgId: String, status: ChatMessage.Status) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(status = status) else it })
        viewModelScope.launch { store?.updateStatus(client.myId, msgId, status) }
    }
    private fun updateFileProgress(peerId: Int, msgId: String, transferred: Int) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(transferred = transferred) else it })
    }

    /** 接收方下载进度：DONE 时 .part 落地为最终文件并写回 Room；FAILED 时清理并提示 */
    private fun onDownloadProgress(e: ImClient.Event.FileProgress) {
        val d = downloads[e.fileId] ?: return
        if (e.status == Protocol.FILE_ST_DONE) {
            viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                runCatching { d.out.close() }
                val ctx = appContext ?: return@launch
                val finalFile = java.io.File(java.io.File(ctx.filesDir, "file"), "${d.fileId}_${d.name}")
                val ok = d.partFile.renameTo(finalFile)
                if (ok) {
                    store?.updateFileLocalPath(client.myId, d.fileId, finalFile.absolutePath)
                }
                kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main) {
                    if (ok) {
                        val conv = _messages.value[d.peerId] ?: return@withContext
                        _messages.value = _messages.value + (d.peerId to conv.map {
                            if (it.fileId == d.fileId) it.copy(localPath = finalFile.absolutePath, status = ChatMessage.Status.RECEIVED) else it })
                    } else {
                        notify("文件保存失败")
                    }
                    downloads.remove(d.fileId)
                }
            }
        } else if (e.status == Protocol.FILE_ST_FAILED) {
            runCatching { d.out.close() }; downloads.remove(e.fileId); notify("文件已失效")
        }
    }

    /**
     * 断线自动重连（指数退避）：弱网/瞬断时自愈，不打断用户停留在会话页。
     * 每轮先建立 TCP 连接，再用保存的密码哈希重新登录；登录成功由 LoginResult 分支清零状态。
     * 达到最大尝试次数仍失败才回登录页。
     */
    private fun startReconnect(tel: String, hash: String) {
        if (reconnecting) return // 已在重连，避免并发
        reconnecting = true
        reconnectJob?.cancel()
        reconnectJob = viewModelScope.launch {
            var attempt = 0
            val maxAttempts = 10
            while (reconnecting && attempt < maxAttempts) {
                attempt++
                val backoffMs = minOf(1000L * (1L shl (attempt - 1)), 15_000L)
                _loginTip.value = "连接已断开，正在重连（第 $attempt 次）…"
                _toast.emit("连接已断开，正在重连…")
                kotlinx.coroutines.delay(backoffMs)
                if (!reconnecting) return@launch // 期间已恢复/被取消
                client.disconnect() // 清掉可能的半开连接
                val ok = runCatching { client.connect(ImClient.DEFAULT_HOST) }.getOrDefault(false)
                if (ok) {
                    // 连接成功即发起重新登录；成功与否由 LoginResult 事件驱动
                    client.loginWithHash(tel, hash)
                    // 给服务端一轮登录往返时间；若成功，reconnecting 会被 LoginResult 置 false
                    kotlinx.coroutines.delay(3000L)
                    if (!reconnecting) return@launch // 登录成功已清零
                    // 否则继续下一轮退避重试
                }
            }
            // 重试用尽仍未恢复：回登录页
            if (reconnecting) {
                reconnecting = false
                _toast.emit("重连失败，请重新登录")
                resetToLogin()
            }
        }
    }

    private fun resetToLogin() {
        reconnecting = false
        reconnectJob?.cancel()
        reconnectJob = null
        loadedMinSeq.clear()
        roamHasMore.clear()
        roamLoading.clear()
        _friends.value = emptyList()
        _messages.value = emptyMap()
        _conversations.value = emptyMap()
        _searchResults.value = emptyList()
        _chatPeer.value = null
        _myNick.value = ""
        _myFeeling.value = ""
        _screen.value = Screen.Login
    }

    override fun onCleared() {
        client.disconnect()
    }

    companion object {
        /** 漫游历史每页条数 */
        private const val PAGE_SIZE = 20
    }
}
