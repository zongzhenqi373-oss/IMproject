#include "client_core/ClientCore.h"
#include "client_core/IStorage.h"
#include "TcpTransport.h"
#include "im.pb.h"
#include "sha256.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace im {

using namespace proto;

namespace {
// pb payload 解析（data/len 为去掉 4B 协议号后的包体）
template <typename T>
bool parsePayload(const char* data, std::size_t len, T& out)
{
    return out.ParseFromArray(data, static_cast<int>(len));
}

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 生成消息唯一 id（msg_id）：时间戳 + 自增计数 + 随机数，漫游/去重/回执关联用
std::string makeMsgId()
{
    static std::atomic<std::uint64_t> counter{0};
    static std::random_device rd;
    const std::uint64_t rand64 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    const std::uint64_t ts = static_cast<std::uint64_t>(steadyNowMs());
    const std::uint64_t seq = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%013llx%05llx%016llx",
                  static_cast<unsigned long long>(ts),
                  static_cast<unsigned long long>(seq & 0xFFFFF),
                  static_cast<unsigned long long>(rand64));
    return buf;
}
} // namespace

ClientCore::ClientCore()
    : m_transport(new TcpTransport)
{
    initFunArr();

    m_transport->setPacketHandler([this](const char* data, std::size_t len) {
        // 任何入站包都刷新活跃时间（含心跳回复）
        m_lastRecvMs.store(steadyNowMs());
        dispatchPacket(data, len);
    });
    m_transport->setCloseHandler([this]() {
        if (auto* ev = m_events.load()) ev->onConnectionClosed();
    });
}

ClientCore::~ClientCore()
{
    // 先停心跳线程（避免其在析构中调用 transport），再断连
    stopHeartbeat();
}

void ClientCore::setEventSink(IClientEvents* events) { m_events.store(events); }
void ClientCore::setStorage(IStorage* storage) { m_storage.store(storage); }

// ---------------- 连接管理 ----------------

bool ClientCore::connectToServer(const std::string& ip, std::uint16_t port)
{
    if (!m_transport->connect(ip, port)) return false;
    m_lastRecvMs.store(steadyNowMs());
    startHeartbeat();
    return true;
}

void ClientCore::disconnect()
{
    stopHeartbeat();
    m_transport->close();
}

bool ClientCore::isConnected() const
{
    return m_transport->isOpen();
}

// ---------------- 协议分发 ----------------

void ClientCore::initFunArr()
{
    m_dealFunArr[DEF_PROT_REGISTER_RS    - DEF_BASE] = &ClientCore::onRegisterRs;
    m_dealFunArr[DEF_PROT_LOGIN_RS       - DEF_BASE] = &ClientCore::onLoginRs;
    m_dealFunArr[DEF_PROT_FRIEND_INFO    - DEF_BASE] = &ClientCore::onFriendInfoPkt;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RS   - DEF_BASE] = &ClientCore::onChatInfoRs;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RQ   - DEF_BASE] = &ClientCore::onChatInfoRq;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RS  - DEF_BASE] = &ClientCore::onAddFriRs;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RQ  - DEF_BASE] = &ClientCore::onAddFriRq;
    m_dealFunArr[DEF_PROT_FRIEND_OFFLINE - DEF_BASE] = &ClientCore::onFriendOfflinePkt;
    m_dealFunArr[DEF_PROT_HEARTBEAT_RS   - DEF_BASE] = &ClientCore::onHeartbeatRs;
    m_dealFunArr[DEF_PROT_KICKED_OFFLINE - DEF_BASE] = &ClientCore::onKickedOfflinePkt;
}

void ClientCore::dispatchPacket(const char* data, std::size_t len)
{
    // 包体 = [4B 小端协议号][pb payload]
    if (!data || len < sizeof(protType)) return;

    const protType type = decodeType32(data);

    // 范围校验（防越界访问函数指针数组）
    if (type < DEF_BASE) return;
    const std::size_t index = type - DEF_BASE;
    if (index >= static_cast<std::size_t>(DEF_PROT_COUNT)) return;

    DealFun pFun = m_dealFunArr[index];
    if (pFun) (this->*pFun)(data + sizeof(protType), len - sizeof(protType));
}

void ClientCore::sendPacket(protType type, const std::string& payload)
{
    std::string body;
    body.resize(sizeof(protType) + payload.size());
    encodeType32(type, body.data());
    std::memcpy(body.data() + sizeof(protType), payload.data(), payload.size());
    m_transport->send(body.data(), body.size());
}

// ---------------- 业务请求 ----------------

void ClientCore::sendRegister(const std::string& nickUtf8, const std::string& tel, const std::string& pass)
{
    im::proto::RegisterRq rq;
    rq.set_nick(utf8Truncate(nickUtf8, USER_NICK_LEN - 1));
    rq.set_tel(utf8Truncate(tel, USER_TEL_LEN - 1));
    // 对齐 QQNT：密码绝不原文上链路，客户端先 SHA-256 一次（固定 64 字符 hex）
    rq.set_pass(sha256Hex(pass));
    sendPacket(DEF_PROT_REGISTER_RQ, rq.SerializeAsString());
}

void ClientCore::sendLogin(const std::string& tel, const std::string& pass)
{
    im::proto::LoginRq rq;
    rq.set_tel(utf8Truncate(tel, USER_TEL_LEN - 1));
    // 对齐 QQNT：传输的是密码哈希，而非明文
    rq.set_pass(sha256Hex(pass));
    sendPacket(DEF_PROT_LOGIN_RQ, rq.SerializeAsString());
}

void ClientCore::sendChatMessage(int friId, const std::string& msgUtf8)
{
    const std::string msg = utf8Truncate(msgUtf8, CHAT_MSG_LEN - 1);

    im::proto::ChatInfoRq rq;
    rq.set_myid(m_myId);
    rq.set_friid(friId);
    rq.set_msg(msg);
    rq.set_type(im::proto::TEXT);
    rq.set_msg_id(makeMsgId());
    sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());

    // 本地持久化：发出的消息
    if (m_myId > 0) {
        if (auto* st = m_storage.load()) {
            st->saveChatMessage(m_myId, friId, true, msg,
                                static_cast<std::int64_t>(std::time(nullptr)));
        }
    }
}

void ClientCore::sendImageMessage(int friId, const std::string& imageBytes, int w, int h)
{
    if (imageBytes.empty()) return;

    im::proto::ChatInfoRq rq;
    rq.set_myid(m_myId);
    rq.set_friid(friId);
    rq.set_type(im::proto::IMAGE);
    rq.set_image_data(imageBytes);
    rq.set_image_width(w);
    rq.set_image_height(h);
    rq.set_msg_id(makeMsgId());
    sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());
}

void ClientCore::sendAddFriendRequest(const std::string& friNickUtf8)
{
    im::proto::AddFriendRq rq;
    rq.set_myid(m_myId);
    rq.set_mynick(m_nick);
    rq.set_frinick(utf8Truncate(friNickUtf8, USER_NICK_LEN - 1));
    sendPacket(DEF_PROT_ADD_FRIEND_RQ, rq.SerializeAsString());
}

void ClientCore::answerAddFriend(int destId, const std::string& destNickUtf8, bool agree)
{
    im::proto::AddFriendRs rs;
    rs.set_result(agree ? ADD_FRIEND_AGREE : ADD_FRIEND_REJECT);
    rs.set_destid(destId);
    rs.set_myid(m_myId);
    rs.set_mynick(m_nick);
    rs.set_destnick(utf8Truncate(destNickUtf8, USER_NICK_LEN - 1));
    sendPacket(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
}

void ClientCore::sendOfflineNotify()
{
    im::proto::FriendOffline pkt;
    pkt.set_offlineid(m_myId);
    sendPacket(DEF_PROT_FRIEND_OFFLINE, pkt.SerializeAsString());
}

// ---------------- 心跳保活 ----------------

void ClientCore::setHeartbeatIntervalMs(int intervalMs)
{
    if (intervalMs > 0) m_hbIntervalMs = intervalMs;
}

void ClientCore::startHeartbeat()
{
    if (m_hbRunning.exchange(true)) return; // 已在运行
    m_hbThread = std::thread([this]() {
        while (m_hbRunning.load()) {
            // 分段睡眠，便于 stopHeartbeat 快速响应
            int waited = 0;
            while (waited < m_hbIntervalMs && m_hbRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waited += 50;
            }
            if (!m_hbRunning.load()) break;
            if (!m_transport->isOpen()) continue;

            // 超时判定：连续 3 个间隔无任何入站数据 → 判定断连
            const std::int64_t last = m_lastRecvMs.load();
            if (last > 0 && steadyNowMs() - last > 3LL * m_hbIntervalMs) {
                m_transport->close(); // 触发 onConnectionClosed
                continue;
            }
            sendPacket(DEF_PROT_HEARTBEAT_RQ, "");
        }
    });
}

void ClientCore::stopHeartbeat()
{
    if (!m_hbRunning.exchange(false)) return;
    if (m_hbThread.joinable()) m_hbThread.join();
}

void ClientCore::onHeartbeatRs(const char*, std::size_t)
{
    // 无需处理：入站包已在 transport 回调中刷新活跃时间
}

void ClientCore::onKickedOfflinePkt(const char*, std::size_t)
{
    // 被踢下线（同账号在别处登录）：通知 UI，由 UI 决定提示与收尾
    if (auto* ev = m_events.load()) ev->onKickedOffline(0);
}

// ---------------- 会话状态 ----------------

int ClientCore::myId() const { return m_myId; }
std::string ClientCore::myNick() const { return m_nick; }
std::string ClientCore::myFeeling() const { return m_feeling; }
int ClientCore::myIconId() const { return m_iconId; }

// ---------------- 协议处理 ----------------
// 注意：m_events/m_storage 为原子指针，先 load 到局部变量再调用，
// 避免"判空"与"解引用"两次独立 load 之间的 TOCTOU。

void ClientCore::onRegisterRs(const char* data, std::size_t len)
{
    im::proto::RegisterRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* ev = m_events.load()) ev->onRegisterResult(rs.result());
}

void ClientCore::onLoginRs(const char* data, std::size_t len)
{
    im::proto::LoginRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (rs.result() == LOGIN_SUCCESS) m_myId = rs.userid();
    if (auto* ev = m_events.load()) ev->onLoginResult(rs.result(), rs.userid());
}

void ClientCore::onFriendInfoPkt(const char* data, std::size_t len)
{
    im::proto::FriendInfo info;
    if (!parsePayload(data, len, info)) return;

    if (info.userid() == m_myId) {
        // 自己的资料：更新会话状态
        m_iconId = info.iconid();
        m_nick = info.nick();
        m_feeling = info.feeling();

        UserInfo self;
        self.id = m_myId;
        self.iconId = m_iconId;
        self.nick = m_nick;
        self.feeling = m_feeling;
        if (auto* st = m_storage.load()) st->saveSelfInfo(self);
        if (auto* ev = m_events.load()) ev->onSelfInfo(self);
    } else {
        im::FriendInfo fri; // 显式限定，避免与 pb 消息 im::proto::FriendInfo 冲突
        fri.id = info.userid();
        fri.iconId = info.iconid();
        fri.status = info.status();
        fri.nick = info.nick();
        fri.feeling = info.feeling();
        if (auto* st = m_storage.load()) st->saveFriend(fri);
        if (auto* ev = m_events.load()) ev->onFriendInfo(fri);
    }
}

void ClientCore::onChatInfoRq(const char* data, std::size_t len)
{
    im::proto::ChatInfoRq rq;
    if (!parsePayload(data, len, rq)) return;

    // 图片消息单独回调（rq.myid 是发送方）
    if (rq.type() == im::proto::IMAGE) {
        if (auto* ev = m_events.load()) {
            ev->onImageMessage(rq.myid(), rq.image_data(), rq.image_width(), rq.image_height(), rq.msg_id());
        }
        return;
    }

    // 本地持久化：收到的文本消息
    if (m_myId > 0) {
        if (auto* st = m_storage.load()) {
            st->saveChatMessage(m_myId, rq.myid(), false, rq.msg(),
                                static_cast<std::int64_t>(std::time(nullptr)));
        }
    }
    if (auto* ev = m_events.load()) ev->onChatMessage(rq.myid(), rq.msg());
}

void ClientCore::onChatInfoRs(const char* data, std::size_t len)
{
    im::proto::ChatInfoRs rs;
    if (!parsePayload(data, len, rs)) return;
    // 回复中 myid 是消息接收方（朋友），friid 是自己
    if (auto* ev = m_events.load()) ev->onChatSendResult(rs.myid(), rs.result());
}

void ClientCore::onAddFriRq(const char* data, std::size_t len)
{
    im::proto::AddFriendRq rq;
    if (!parsePayload(data, len, rq)) return;
    if (auto* ev = m_events.load()) ev->onAddFriendRequest(rq.myid(), rq.mynick());
}

void ClientCore::onAddFriRs(const char* data, std::size_t len)
{
    im::proto::AddFriendRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* ev = m_events.load()) ev->onAddFriendResult(rs.result(), rs.mynick());
}

void ClientCore::onFriendOfflinePkt(const char* data, std::size_t len)
{
    im::proto::FriendOffline pkt;
    if (!parsePayload(data, len, pkt)) return;
    if (auto* ev = m_events.load()) ev->onFriendOffline(pkt.offlineid());
}

} // namespace im
