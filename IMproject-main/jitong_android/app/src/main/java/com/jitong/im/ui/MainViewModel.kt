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

enum class MsgKind { TEXT, IMAGE }

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

    init {
        viewModelScope.launch { collectEvents() }
    }

    /** MainActivity 注入仓储后触发：MMKV 有凭证则自动登录 */
    fun attachStore(store: ChatStore) {
        if (this.store != null) return
        this.store = store
        val tel = Prefs.tel
        val hash = Prefs.passHash
        if (!tel.isNullOrEmpty() && !hash.isNullOrEmpty()) {
            lastTel = tel
            lastHash = hash
            viewModelScope.launch {
                _loginTip.value = "自动登录中…"
                if (!client.connect(ImClient.DEFAULT_HOST)) {
                    _loginTip.value = "连接失败，请确认 im_server 已启动"
                    return@launch
                }
                client.loginWithHash(tel, hash)
            }
        }
    }

    // ---------------- 登录 / 注册 ----------------

    fun login(tel: String, pass: String) {
        if (tel.isBlank() || pass.isBlank()) {
            _loginTip.value = "请输入手机号和密码"
            return
        }
        lastTel = tel.trim()
        lastHash = sha256Hex(pass)
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
    }

    fun backToFriends() {
        _screen.value = Screen.FriendList
    }

    fun logout() {
        expectDisconnect = true
        Prefs.clearCredentials()
        viewModelScope.launch { client.logout() }
        resetToLogin()
    }

    // ---------------- 聊天 ----------------

    fun send(text: String) {
        val peer = _chatPeer.value ?: return
        if (text.isBlank()) return
        val msgId = UUID.randomUUID().toString()
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
                        // 保存凭证（自动登录）并从本地库装载历史
                        lastTel?.let { Prefs.tel = it }
                        lastHash?.let { Prefs.passHash = it }
                        _screen.value = Screen.FriendList
                        store?.let { s ->
                            viewModelScope.launch {
                                val (msgs, convs) = s.hydrate(client.myId)
                                _messages.value = msgs
                                _conversations.value = convs
                            }
                        }
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
                    append(
                        ChatMessage(e.msgId, e.fromId, fromMe = false, text = e.text),
                        incrUnread = !inChat,
                    )
                }

                is ImClient.Event.ImageReceived -> {
                    val inChat = _screen.value == Screen.Chat && _chatPeer.value?.id == e.fromId
                    append(
                        ChatMessage(
                            e.msgId, e.fromId, fromMe = false, kind = MsgKind.IMAGE,
                            imageBytes = e.bytes, imgW = e.w, imgH = e.h,
                        ),
                        incrUnread = !inChat,
                    )
                }

                is ImClient.Event.ChatSendResult -> updateStatus(
                    e.peerId, e.msgId,
                    if (e.result == Protocol.CHAT_RESULT_SUCC)
                        ChatMessage.Status.DELIVERED else ChatMessage.Status.OFFLINE_STORED
                )

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
                    Prefs.clearCredentials()
                    client.disconnect()
                    _toast.emit("你的账号已在其他设备登录")
                    resetToLogin()
                }

                ImClient.Event.Disconnected -> {
                    if (!expectDisconnect && _screen.value != Screen.Login) {
                        _toast.emit("与服务器断开连接")
                        resetToLogin()
                    }
                    expectDisconnect = false
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

    /** msg_id 去重（内存 + Room UNIQUE 双保险），并按需落库 */
    private fun append(msg: ChatMessage, incrUnread: Boolean) {
        val conv = _messages.value[msg.peerId].orEmpty()
        if (conv.any { it.msgId == msg.msgId }) return
        _messages.value = _messages.value + (msg.peerId to conv + msg)

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

        viewModelScope.launch { store?.save(client.myId, msg, incrUnread) }
    }

    private fun updateStatus(peerId: Int, msgId: String, status: ChatMessage.Status) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(status = status) else it
        })
        viewModelScope.launch { store?.updateStatus(client.myId, msgId, status) }
    }

    private fun resetToLogin() {
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
}
