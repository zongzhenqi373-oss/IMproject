#include "Dispatcher.h"
#include "Server.h"
#include "Session.h"
#include "Log.h"
#include "client_core/Protocol.h"
#include "im.pb.h"
#include "sha256.h"
#include "image_format.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace imsrv {

using namespace im::proto;

namespace {
template <typename T>
bool parsePayload(const std::string& payload, T& out)
{
    return out.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}

std::int64_t nowSec() { return static_cast<std::int64_t>(std::time(nullptr)); }

// 老客户端可能不传 msg_id：服务端兜底生成（否则 msg_id UNIQUE 约束会让空串消息互相 IGNORE）
std::string fallbackMsgId(int sender, int receiver, const std::string& content)
{
    static std::atomic<std::uint64_t> counter{0};
    return im::sha256Hex(std::to_string(sender) + "|" + std::to_string(receiver) + "|" +
                         std::to_string(nowSec()) + "|" + std::to_string(counter.fetch_add(1)) + "|" + content);
}
} // namespace

Dispatcher::Dispatcher(Server& server)
    : m_server(server)
{
    m_handlers[DEF_PROT_REGISTER_RQ]    = [this](auto& s, auto& p) { onRegisterRq(s, p); };
    m_handlers[DEF_PROT_LOGIN_RQ]       = [this](auto& s, auto& p) { onLoginRq(s, p); };
    m_handlers[DEF_PROT_CHAT_INFO_RQ]   = [this](auto& s, auto& p) { onChatRq(s, p); };
    m_handlers[DEF_PROT_FRIEND_OFFLINE] = [this](auto& s, auto& p) { onOfflineRq(s, p); };
    m_handlers[DEF_PROT_HEARTBEAT_RQ]   = [this](auto& s, auto& p) { onHeartbeatRq(s, p); };
    m_handlers[DEF_PROT_ADD_FRIEND_RQ]  = [this](auto& s, auto& p) { onAddFriendRq(s, p); };
    m_handlers[DEF_PROT_ADD_FRIEND_RS]  = [this](auto& s, auto& p) { onAddFriendRs(s, p); };
}

void Dispatcher::handle(const std::shared_ptr<Session>& s, std::uint32_t type, const std::string& payload)
{
    auto it = m_handlers.find(type);
    if (it == m_handlers.end()) {
        log("[dispatcher] 未注册协议号: ", type);
        return;
    }
    it->second(s, payload);
}

// ---------------- 注册 ----------------

void Dispatcher::onRegisterRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    RegisterRq rq;
    if (!parsePayload(payload, rq)) return;

    const int rc = m_server.db().registerUser(
        utf8Truncate(rq.nick(), USER_NICK_LEN - 1),
        utf8Truncate(rq.tel(), USER_TEL_LEN - 1),
        rq.pass()); // 客户端已哈希

    RegisterRs rs;
    rs.set_result(rc);
    s->deliver(DEF_PROT_REGISTER_RS, rs.SerializeAsString());
    log("[业务] 注册 nick=", rq.nick(), " 结果=", rc);
}

// ---------------- 登录 ----------------

void Dispatcher::sendFriendInfo(const std::shared_ptr<Session>& to, int userId, int onlineStatus)
{
    UserRecord u;
    if (!m_server.db().getUser(userId, u)) return;

    FriendInfo info;
    info.set_userid(u.id);
    info.set_iconid(u.iconId);
    info.set_status(onlineStatus);
    info.set_nick(u.nick);
    info.set_feeling(u.feeling);
    to->deliver(DEF_PROT_FRIEND_INFO, info.SerializeAsString());
}

void Dispatcher::onLoginRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    LoginRq rq;
    if (!parsePayload(payload, rq)) return;

    int userId = 0;
    const int rc = m_server.db().loginUser(utf8Truncate(rq.tel(), USER_TEL_LEN - 1), rq.pass(), userId);

    if (rc != LOGIN_SUCCESS) {
        LoginRs rs;
        rs.set_result(rc);
        s->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        log("[业务] 登录失败 tel=", rq.tel(), " 结果=", rc);
        return;
    }

    // 多端互踢（对齐 QQNT kickoff）：先通知旧端，覆盖映射，再关旧连接
    if (auto old = m_server.presence().get(userId); old && old != s) {
        old->deliver(DEF_PROT_KICKED_OFFLINE, "");
        m_server.presence().online(userId, s);
        old->close();
        log("[业务] 用户 id=", userId, " 在别处登录，旧连接被踢");
    } else {
        m_server.presence().online(userId, s);
    }
    s->setUserId(userId);

    LoginRs rs;
    rs.set_userid(userId);
    rs.set_result(rc);
    s->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
    log("[业务] 登录成功 id=", userId);

    // 自资料下发
    sendFriendInfo(s, userId, STATUS_ONLINE);

    // 好友资料下发 + 通知在线好友"我上线了"
    for (const auto& f : m_server.db().getFriends(userId)) {
        const bool fOnline = m_server.presence().isOnline(f.id);
        sendFriendInfo(s, f.id, fOnline ? STATUS_ONLINE : STATUS_OFFLINE);
        if (auto fs = m_server.presence().get(f.id)) {
            sendFriendInfo(fs, userId, STATUS_ONLINE);
        }
    }

    // 离线消息补发（文本直接回传；图片读文件回传字节）
    auto undelivered = m_server.db().pullUndelivered(userId);
    std::vector<std::string> deliveredIds;
    for (const auto& m : undelivered) {
        ChatInfoRq out;
        out.set_myid(m.senderId);
        out.set_friid(userId);
        out.set_type(m.type == 1 ? IMAGE : TEXT);
        out.set_msg_id(m.msgId);
        if (m.type == 0) {
            out.set_msg(m.content);
        } else if (m.type == 1 && !m.mediaPath.empty()) {
            std::ifstream ifs(m.mediaPath, std::ios::binary);
            if (!ifs) continue; // 文件丢失则跳过该条
            std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            out.set_image_data(bytes);
            out.set_image_width(m.imgW);
            out.set_image_height(m.imgH);
        }
        s->deliver(DEF_PROT_CHAT_INFO_RQ, out.SerializeAsString());
        deliveredIds.push_back(m.msgId);

        // 送达回执：原发送方在线则通知其把"对方离线，已转存"刷新为"已送达"
        // （复用 ChatInfoRs 语义：myid=接收方好友，friid=回执去向即原发送方，关联 msg_id）
        if (auto sender = m_server.presence().get(m.senderId)) {
            ChatInfoRs receipt;
            receipt.set_myid(userId);
            receipt.set_friid(m.senderId);
            receipt.set_result(CHAT_RESULT_SUCC);
            receipt.set_msg_id(m.msgId);
            sender->deliver(DEF_PROT_CHAT_INFO_RS, receipt.SerializeAsString());
        }
    }
    m_server.db().markDelivered(deliveredIds);
    if (!undelivered.empty()) {
        log("[业务] 离线消息补发 id=", userId, " 条数=", undelivered.size());
    }
}

// ---------------- 聊天 ----------------

std::string Dispatcher::saveImage(const std::string& bytes)
{
    const std::string name = im::sha256Hex(bytes) + im::imageExtForBytes(bytes);
    const std::string rel = m_server.uploadDir() + "/img/" + name;
    std::error_code ec;
    std::filesystem::create_directories(m_server.uploadDir() + "/img", ec);
    if (std::filesystem::exists(rel, ec)) return rel; // 哈希去重
    std::ofstream ofs(rel, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return rel;
}

void Dispatcher::onChatRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    ChatInfoRq rq;
    if (!parsePayload(payload, rq)) return;

    // 图片先落盘（db 只存路径，不存 blob）
    std::string mediaPath;
    if (rq.type() == IMAGE && !rq.image_data().empty()) {
        mediaPath = saveImage(rq.image_data());
    }

    StoredMessage m;
    m.msgId = rq.msg_id().empty() ? fallbackMsgId(rq.myid(), rq.friid(), rq.msg()) : rq.msg_id();
    m.senderId = rq.myid();
    m.receiverId = rq.friid();
    m.type = rq.type() == IMAGE ? 1 : 0;
    m.content = rq.msg();
    m.mediaPath = mediaPath;
    m.imgW = rq.image_width();
    m.imgH = rq.image_height();
    m.ts = nowSec();

    ChatInfoRs rs;
    rs.set_myid(rq.friid());
    rs.set_friid(rq.myid());
    rs.set_msg_id(rq.msg_id());

    if (auto target = m_server.presence().get(rq.friid())) {
        // 在线：存历史（已投递）+ 原样转发（含图片字节）
        m_server.db().saveMessage(m, true);
        target->deliver(DEF_PROT_CHAT_INFO_RQ, payload);
        rs.set_result(CHAT_RESULT_SUCC);
    } else {
        // 离线：存历史（未投递），上线补发
        m_server.db().saveMessage(m, false);
        rs.set_result(CHAT_RESULT_FAIL);
    }
    s->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
}

// ---------------- 下线 ----------------

void Dispatcher::onOfflineRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    FriendOffline pkt;
    if (!parsePayload(payload, pkt)) return;

    // 摘映射（onSessionClosed 会广播好友下线，这里只需关连接）
    m_server.presence().offline(pkt.offlineid(), s);
    s->close();
}

// ---------------- 心跳 ----------------

void Dispatcher::onHeartbeatRq(const std::shared_ptr<Session>& s, const std::string&)
{
    if (s->userId() > 0) m_server.presence().touch(s->userId());
    s->deliver(DEF_PROT_HEARTBEAT_RS, "");
}

// ---------------- 添加好友 ----------------

void Dispatcher::onAddFriendRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    AddFriendRq rq;
    if (!parsePayload(payload, rq)) return;

    const int friendId = m_server.db().getUserIdByNick(utf8Truncate(rq.frinick(), USER_NICK_LEN - 1));
    if (friendId == 0) {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_NOTEXIT);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        return;
    }

    if (auto target = m_server.presence().get(friendId)) {
        target->deliver(DEF_PROT_ADD_FRIEND_RQ, payload); // 在线则转发
    } else {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_OFFLINE);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
    }
}

void Dispatcher::onAddFriendRs(const std::shared_ptr<Session>& s, const std::string& payload)
{
    AddFriendRs rs;
    if (!parsePayload(payload, rs)) return;

    if (rs.result() == ADD_FRIEND_AGREE) {
        m_server.db().addFriendBidirectional(rs.destid(), rs.myid());
        // 刷新双方资料/好友列表
        sendFriendInfo(s, rs.myid(), STATUS_ONLINE);
        for (const auto& f : m_server.db().getFriends(rs.myid())) {
            sendFriendInfo(s, f.id, m_server.presence().isOnline(f.id) ? STATUS_ONLINE : STATUS_OFFLINE);
        }
        if (auto dest = m_server.presence().get(rs.destid())) {
            sendFriendInfo(dest, rs.destid(), STATUS_ONLINE);
            for (const auto& f : m_server.db().getFriends(rs.destid())) {
                sendFriendInfo(dest, f.id, m_server.presence().isOnline(f.id) ? STATUS_ONLINE : STATUS_OFFLINE);
            }
        }
    }

    // 回复转发给发起方
    if (auto dest = m_server.presence().get(rs.destid())) {
        dest->deliver(DEF_PROT_ADD_FRIEND_RS, payload);
    }
}

} // namespace imsrv
