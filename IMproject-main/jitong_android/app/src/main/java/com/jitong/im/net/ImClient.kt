package com.jitong.im.net

import im.proto.Im
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * 即通 Android 纯 Kotlin 协议栈客户端（设计决策 D2：本期不引入 djinni/JNI）。
 * 职责：TCP 连接管理、二段式帧收发、心跳保活、协议分发。
 * 事件通过 [events] 流出，UI 层在 ViewModel 中收集（自动切主线程）。
 */
class ImClient {

    sealed interface Event {
        data class RegisterResult(val result: Int) : Event
        data class LoginResult(val result: Int, val userId: Int) : Event

        /** 本人资料（userId == myId）或好友资料/上下线状态刷新 */
        data class UserOrFriendInfo(
            val userId: Int,
            val nick: String,
            val feeling: String,
            val online: Boolean,
        ) : Event

        /** 收到文本消息（在线转发或离线补发）；图片消息属 M5 范围，本期忽略 */
        data class ChatReceived(val fromId: Int, val text: String, val msgId: String) : Event

        /** 发送回执：peerId=接收方好友，result=CHAT_RESULT_SUCC(已送达)/FAIL(已转存离线) */
        data class ChatSendResult(val peerId: Int, val result: Int, val msgId: String) : Event

        data class FriendOffline(val userId: Int) : Event

        /** 账号在别处登录，被服务端踢下线 */
        data object KickedOffline : Event

        /** 连接断开（含主动 logout 之外的意外断开） */
        data object Disconnected : Event
    }

    private val _events = MutableSharedFlow<Event>(extraBufferCapacity = 64)
    val events: SharedFlow<Event> = _events

    @Volatile
    var myId: Int = 0
        private set

    @Volatile
    var connected: Boolean = false
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var socket: Socket? = null
    private val writeLock = Mutex()
    private var heartbeatJob: kotlinx.coroutines.Job? = null

    /** 建立 TCP 连接并启动读循环 + 心跳；成功返回 true */
    suspend fun connect(host: String, port: Int = Protocol.TCP_PORT): Boolean =
        withContext(Dispatchers.IO) {
            if (connected) return@withContext true
            try {
                val s = Socket()
                s.connect(InetSocketAddress(host, port), 5000)
                s.tcpNoDelay = true
                s.keepAlive = true
                socket = s
                connected = true
                startHeartbeat()
                scope.launch { readLoop(s) }
                true
            } catch (e: Exception) {
                closeQuietly()
                false
            }
        }

    suspend fun register(nick: String, tel: String, pass: String) {
        val rq = Im.RegisterRq.newBuilder()
            .setNick(nick)
            .setTel(tel)
            .setPass(sha256Hex(pass)) // 与 C++ 端一致：哈希上链路
            .build()
        send(Protocol.REGISTER_RQ, rq.toByteArray())
    }

    suspend fun login(tel: String, pass: String) {
        val rq = Im.LoginRq.newBuilder()
            .setTel(tel)
            .setPass(sha256Hex(pass))
            .build()
        send(Protocol.LOGIN_RQ, rq.toByteArray())
    }

    suspend fun sendChat(friId: Int, text: String, msgId: String) {
        val rq = Im.ChatInfoRq.newBuilder()
            .setMyid(myId)
            .setFriid(friId)
            .setMsg(text)
            .setType(Im.MsgType.TEXT)
            .setMsgId(msgId) // 客户端生成 UUID，漫游/去重幂等
            .build()
        send(Protocol.CHAT_INFO_RQ, rq.toByteArray())
    }

    /** 主动下线：通知服务端（其会广播好友）后关闭连接 */
    suspend fun logout() {
        if (connected && myId > 0) {
            val pkt = Im.FriendOffline.newBuilder().setOfflineid(myId).build()
            try {
                send(Protocol.FRIEND_OFFLINE, pkt.toByteArray())
            } catch (_: Exception) {
            }
        }
        disconnect()
    }

    fun disconnect() {
        heartbeatJob?.cancel()
        closeQuietly()
    }

    // ---------------- 内部实现 ----------------

    private suspend fun readLoop(s: Socket) {
        try {
            val input = DataInputStream(s.getInputStream())
            while (currentCoroutineContext().isActive) {
                val frame = Frame.readFrom(input) ?: break
                dispatch(frame)
            }
        } catch (_: Exception) {
            // 读异常（对端关闭/网络错误）统一走断连收尾
        } finally {
            closeQuietly()
            _events.emit(Event.Disconnected)
        }
    }

    private suspend fun dispatch(f: Frame) {
        when (f.type) {
            Protocol.REGISTER_RS ->
                _events.emit(Event.RegisterResult(Im.RegisterRs.parseFrom(f.payload).result))

            Protocol.LOGIN_RS -> {
                val rs = Im.LoginRs.parseFrom(f.payload)
                if (rs.result == Protocol.LOGIN_SUCCESS) myId = rs.userid
                _events.emit(Event.LoginResult(rs.result, rs.userid))
            }

            Protocol.FRIEND_INFO -> {
                val info = Im.FriendInfo.parseFrom(f.payload)
                _events.emit(
                    Event.UserOrFriendInfo(
                        info.userid, info.nick, info.feeling,
                        info.status == Protocol.STATUS_ONLINE,
                    )
                )
            }

            Protocol.CHAT_INFO_RQ -> {
                val rq = Im.ChatInfoRq.parseFrom(f.payload)
                if (rq.type == Im.MsgType.TEXT) {
                    _events.emit(Event.ChatReceived(rq.myid, rq.msg, rq.msgId))
                }
            }

            Protocol.CHAT_INFO_RS -> {
                val rs = Im.ChatInfoRs.parseFrom(f.payload)
                // 回执里 myid=接收方好友，friid=自己
                _events.emit(Event.ChatSendResult(rs.myid, rs.result, rs.msgId))
            }

            Protocol.FRIEND_OFFLINE ->
                _events.emit(Event.FriendOffline(Im.FriendOffline.parseFrom(f.payload).offlineid))

            Protocol.KICKED_OFFLINE -> _events.emit(Event.KickedOffline)

            Protocol.HEARTBEAT_RS -> Unit // 心跳应答，无需处理
        }
    }

    private fun startHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive) {
                delay(HEARTBEAT_INTERVAL_MS)
                try {
                    send(Protocol.HEARTBEAT_RQ, ByteArray(0))
                } catch (_: Exception) {
                    break // 发送失败说明连接已坏，交由读循环收尾
                }
            }
        }
    }

    // 所有发送集中走 IO 线程：UI 层在主线程直接调用也安全（NetworkOnMainThreadException 防护）
    private suspend fun send(type: Int, payload: ByteArray) = withContext(Dispatchers.IO) {
        val s = socket ?: throw java.io.IOException("未连接")
        val bytes = Frame.encode(type, payload)
        writeLock.withLock {
            s.getOutputStream().write(bytes)
            s.getOutputStream().flush()
        }
    }

    private fun closeQuietly() {
        connected = false
        heartbeatJob?.cancel()
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
    }

    companion object {
        /** 30s 一次心跳，与服务端 90s 超时空窗对齐 client_core */
        const val HEARTBEAT_INTERVAL_MS = 30_000L
    }
}
