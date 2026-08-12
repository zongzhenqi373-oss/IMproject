#include "client_core/ClientCore.h"
#include "client_core/IStorage.h"
#include "TcpTransport.h"
#include "im.pb.h"

#include <ctime>

namespace im {

using namespace proto;

namespace {
// pb payload 解析（data/len 为去掉 4B 协议号后的包体）
template <typename T>
bool parsePayload(const char* data, std::size_t len, T& out)
{
    return out.ParseFromArray(data, static_cast<int>(len));
}
} // namespace

ClientCore::ClientCore()
    : m_transport(new TcpTransport)
{
    initFunArr();

    m_transport->setPacketHandler([this](const char* data, std::size_t len) {
        dispatchPacket(data, len);
    });
    m_transport->setCloseHandler([this]() {
        if (m_events) m_events->onConnectionClosed();
    });
}

ClientCore::~ClientCore() = default;

void ClientCore::setEventSink(IClientEvents* events) { m_events = events; }
void ClientCore::setStorage(IStorage* storage) { m_storage = storage; }

// ---------------- 连接管理 ----------------

bool ClientCore::connectToServer(const std::string& ip, std::uint16_t port)
{
    return m_transport->connect(ip, port);
}

void ClientCore::disconnect()
{
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
    rq.set_pass(utf8Truncate(pass, USER_PASS_LEN - 1));
    sendPacket(DEF_PROT_REGISTER_RQ, rq.SerializeAsString());
}

void ClientCore::sendLogin(const std::string& tel, const std::string& pass)
{
    im::proto::LoginRq rq;
    rq.set_tel(utf8Truncate(tel, USER_TEL_LEN - 1));
    rq.set_pass(utf8Truncate(pass, USER_PASS_LEN - 1));
    sendPacket(DEF_PROT_LOGIN_RQ, rq.SerializeAsString());
}

void ClientCore::sendChatMessage(int friId, const std::string& msgUtf8)
{
    const std::string msg = utf8Truncate(msgUtf8, CHAT_MSG_LEN - 1);

    im::proto::ChatInfoRq rq;
    rq.set_myid(m_myId);
    rq.set_friid(friId);
    rq.set_msg(msg);
    sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());

    // 本地持久化：发出的消息
    if (m_storage && m_myId > 0) {
        m_storage->saveChatMessage(m_myId, friId, true, msg,
                                   static_cast<std::int64_t>(std::time(nullptr)));
    }
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

// ---------------- 会话状态 ----------------

int ClientCore::myId() const { return m_myId; }
std::string ClientCore::myNick() const { return m_nick; }
std::string ClientCore::myFeeling() const { return m_feeling; }
int ClientCore::myIconId() const { return m_iconId; }

// ---------------- 协议处理 ----------------

void ClientCore::onRegisterRs(const char* data, std::size_t len)
{
    im::proto::RegisterRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (m_events) m_events->onRegisterResult(rs.result());
}

void ClientCore::onLoginRs(const char* data, std::size_t len)
{
    im::proto::LoginRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (rs.result() == LOGIN_SUCCESS) m_myId = rs.userid();
    if (m_events) m_events->onLoginResult(rs.result(), rs.userid());
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
        if (m_storage) m_storage->saveSelfInfo(self);
        if (m_events) m_events->onSelfInfo(self);
    } else {
        im::FriendInfo fri; // 显式限定，避免与 pb 消息 im::proto::FriendInfo 冲突
        fri.id = info.userid();
        fri.iconId = info.iconid();
        fri.status = info.status();
        fri.nick = info.nick();
        fri.feeling = info.feeling();
        if (m_storage) m_storage->saveFriend(fri);
        if (m_events) m_events->onFriendInfo(fri);
    }
}

void ClientCore::onChatInfoRq(const char* data, std::size_t len)
{
    im::proto::ChatInfoRq rq;
    if (!parsePayload(data, len, rq)) return;

    // 本地持久化：收到的消息（rq.myid 是发送方）
    if (m_storage && m_myId > 0) {
        m_storage->saveChatMessage(m_myId, rq.myid(), false, rq.msg(),
                                   static_cast<std::int64_t>(std::time(nullptr)));
    }
    if (m_events) m_events->onChatMessage(rq.myid(), rq.msg());
}

void ClientCore::onChatInfoRs(const char* data, std::size_t len)
{
    im::proto::ChatInfoRs rs;
    if (!parsePayload(data, len, rs)) return;
    // 回复中 myid 是消息接收方（朋友），friid 是自己
    if (m_events) m_events->onChatSendResult(rs.myid(), rs.result());
}

void ClientCore::onAddFriRq(const char* data, std::size_t len)
{
    im::proto::AddFriendRq rq;
    if (!parsePayload(data, len, rq)) return;
    if (m_events) m_events->onAddFriendRequest(rq.myid(), rq.mynick());
}

void ClientCore::onAddFriRs(const char* data, std::size_t len)
{
    im::proto::AddFriendRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (m_events) m_events->onAddFriendResult(rs.result(), rs.mynick());
}

void ClientCore::onFriendOfflinePkt(const char* data, std::size_t len)
{
    im::proto::FriendOffline pkt;
    if (!parsePayload(data, len, pkt)) return;
    if (m_events) m_events->onFriendOffline(pkt.offlineid());
}

} // namespace im
