package com.jitong.im.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.jitong.im.net.ImClient
import com.jitong.im.net.Protocol
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

    /** 当前登录账号 id（未登录为 0） */
    val myId: Int get() = client.myId

    /** 轻提示（文件传输占位等场景） */
    fun notify(msg: String) {
        viewModelScope.launch { _toast.emit(msg) }
    }

    private val _screen = MutableStateFlow(Screen.Login)
    val screen: StateFlow<Screen> = _screen

    private val _loginTip = MutableStateFlow("")
    val loginTip: StateFlow<String> = _loginTip

    private val _myNick = MutableStateFlow("")
    val myNick: StateFlow<String> = _myNick

    private val _myFeeling = MutableStateFlow("")
    val myFeeling: StateFlow<String> = _myFeeling

    /** 好友列表：在线的排前面（每次刷新重排） */
    private val _friends = MutableStateFlow<List<Friend>>(emptyList())
    val friends: StateFlow<List<Friend>> = _friends

    /** 全部会话消息：peerId -> 有序消息列表（M4 内存态，Room 持久化属 M5） */
    private val _messages = MutableStateFlow<Map<Int, List<ChatMessage>>>(emptyMap())
    val messages: StateFlow<Map<Int, List<ChatMessage>>> = _messages

    /** 当前聊天对象 */
    private val _chatPeer = MutableStateFlow<Friend?>(null)
    val chatPeer: StateFlow<Friend?> = _chatPeer

    /** 一次性提示（被踢下线/断连等） */
    private val _toast = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val toast: SharedFlow<String> = _toast

    private var expectDisconnect = false // 主动 logout/被踢时不弹"断开连接"

    init {
        viewModelScope.launch { collectEvents() }
    }

    // ---------------- 登录 / 注册 ----------------

    fun login(tel: String, pass: String) {
        if (tel.isBlank() || pass.isBlank()) {
            _loginTip.value = "请输入手机号和密码"
            return
        }
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
    }

    fun backToFriends() {
        _screen.value = Screen.FriendList
    }

    fun logout() {
        expectDisconnect = true
        viewModelScope.launch { client.logout() }
        resetToLogin()
    }

    // ---------------- 聊天 ----------------

    fun send(text: String) {
        val peer = _chatPeer.value ?: return
        if (text.isBlank()) return
        val msgId = UUID.randomUUID().toString()
        append(ChatMessage(msgId, peer.id, fromMe = true, text = text, status = ChatMessage.Status.SENDING))
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
            )
        )
        viewModelScope.launch { client.sendImage(peer.id, bytes, w, h, msgId) }
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
                        _screen.value = Screen.FriendList
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
                        // 聊天页顶部在线状态联动
                        _chatPeer.value?.takeIf { it.id == e.userId }?.let {
                            _chatPeer.value = it.copy(online = e.online)
                        }
                    }
                }

                is ImClient.Event.ChatReceived ->
                    append(ChatMessage(e.msgId, e.fromId, fromMe = false, text = e.text))

                is ImClient.Event.ImageReceived ->
                    append(
                        ChatMessage(
                            e.msgId, e.fromId, fromMe = false, kind = MsgKind.IMAGE,
                            imageBytes = e.bytes, imgW = e.w, imgH = e.h,
                        )
                    )

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

    /** msg_id 去重：离线补发/重传场景下幂等合并（对齐设计 D7） */
    private fun append(msg: ChatMessage) {
        val conv = _messages.value[msg.peerId].orEmpty()
        if (conv.any { it.msgId == msg.msgId }) return
        _messages.value = _messages.value + (msg.peerId to conv + msg)
    }

    private fun updateStatus(peerId: Int, msgId: String, status: ChatMessage.Status) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(status = status) else it
        })
    }

    private fun resetToLogin() {
        _friends.value = emptyList()
        _messages.value = emptyMap()
        _chatPeer.value = null
        _myNick.value = ""
        _screen.value = Screen.Login
    }

    override fun onCleared() {
        client.disconnect()
    }
}
