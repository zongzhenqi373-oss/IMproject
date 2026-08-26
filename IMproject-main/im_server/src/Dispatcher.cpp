#include "Dispatcher.h"
#include "Server.h"
#include "Session.h"
#include "Log.h"
#include "client_core/Protocol.h"
#include "im.pb.h"
#include "sha256.h"
#include "image_format.h"
#include "auth/TokenService.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
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

// 把一条 StoredMessage 填入 ChatInfoRq（漫游/补发共用口径：myid=发送方, friid=接收方, 带 ts/seq）。
// withImage=false：图片消息只填 type，不读盘、不带字节（会话列表预览用）。
// withImage=true：图片读盘回传完整字节；文件丢失返回 false，调用方跳过该条。
bool fillChatInfo(im::proto::ChatInfoRq& out, const StoredMessage& m, bool withImage)
{
    out.set_myid(m.senderId);
    out.set_friid(m.receiverId);
    out.set_msg_id(m.msgId);
    out.set_ts(m.ts);
    out.set_seq(m.seq);
    if (m.type == 1) {
        out.set_type(im::proto::IMAGE);
        out.set_image_width(m.imgW);
        out.set_image_height(m.imgH);
        if (withImage) {
            if (m.mediaPath.empty()) return false;
            std::ifstream ifs(m.mediaPath, std::ios::binary);
            if (!ifs) return false;
            std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            out.set_image_data(bytes);
        }
    } else if (m.type == 2) {
        out.set_type(im::proto::FILE);
        out.set_file_id(m.fileId);
        out.set_file_name(m.content);
        out.set_file_size(m.fileSize);
    } else if (m.type == 0){
        out.set_type(im::proto::TEXT);
        out.set_msg(m.content);
    }
    return true;
}
} // namespace

Dispatcher::Dispatcher(Server& server)
    : m_server(server)
{
    m_handlers[DEF_PROT_REGISTER_RQ]        = [this](auto& s, auto& p) { onRegisterRq(s, p); };
    m_handlers[DEF_PROT_LOGIN_RQ]           = [this](auto& s, auto& p) { onLoginRq(s, p); };
    m_handlers[DEF_PROT_TOKEN_LOGIN_RQ]     = [this](auto& s, auto& p) { onTokenLoginRq(s, p); };
    m_handlers[DEF_PROT_TOKEN_REFRESH_RQ]   = [this](auto& s, auto& p) { onTokenRefreshRq(s, p); };
    m_handlers[DEF_PROT_LOGOUT_RQ]          = [this](auto& s, auto& p) { onLogoutRq(s, p); };
    m_handlers[DEF_PROT_CHAT_INFO_RQ]       = [this](auto& s, auto& p) { onChatRq(s, p); };
    m_handlers[DEF_PROT_FRIEND_OFFLINE]     = [this](auto& s, auto& p) { onOfflineRq(s, p); };
    m_handlers[DEF_PROT_HEARTBEAT_RQ]       = [this](auto& s, auto& p) { onHeartbeatRq(s, p); };
    m_handlers[DEF_PROT_ADD_FRIEND_RQ]      = [this](auto& s, auto& p) { onAddFriendRq(s, p); };
    m_handlers[DEF_PROT_ADD_FRIEND_RS]      = [this](auto& s, auto& p) { onAddFriendRs(s, p); };
    m_handlers[DEF_PROT_ROAM_CONV_RQ]       = [this](auto& s, auto& p) { onRoamConvRq(s, p); };
    m_handlers[DEF_PROT_ROAM_MSG_RQ]        = [this](auto& s, auto& p) { onRoamMsgRq(s, p); };
    m_handlers[DEF_PROT_FILE_OFFER_RQ]      = [this](auto& s, auto& p) { onFileOfferRq(s, p); };
    m_handlers[DEF_PROT_FILE_CHUNK_RQ]      = [this](auto& s, auto& p) { onFileChunkRq(s, p); };
    m_handlers[DEF_PROT_FILE_COMPLETE_RQ]   = [this](auto& s, auto& p) { onFileCompleteRq(s, p); };
    m_handlers[DEF_PROT_FILE_DOWNLOAD_RQ]   = [this](auto& s, auto& p) { onFileDownloadRq(s, p); };
}

void Dispatcher::handle(const std::shared_ptr<Session>& s, std::uint32_t type, const std::string& payload)
{
    auto it = m_handlers.find(type);
    if (it == m_handlers.end()) {
        log("[dispatcher] 未注册协议号: ", type);
        return;
    }
    //增加异常边界，当前只有密码登录的issue()有try/catch，Token登录、刷新、注销仍有可能抛异常
    /*单连接业务异常 → 只关闭当前 Session → 不让异常逃到 Asio 线程池 → 不让整个服务端 std::terminate*/
    try {
        it->second(s, payload);
    } catch (const std::exception& e) {
        log("[dispatcher] 协议处理异常: ", type, " error=", e.what());
        s->close();
    }catch (...) {
        log("[dispatcher] 协议处理未知异常: ", type);
        s->close();
    }
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

void Dispatcher::activateAuthenticatedSession(const std::shared_ptr<Session>& s, int userId)
{
    // 原子替换：避免"先 get() 再 online()"两步分开时，两个连接并发登录同一账号
    // 都读到旧连接为空/是自己，谁都没被踢下线，最后出现两个同时存活的会话
    if (auto old = m_server.presence().replace(userId, s); old && old != s) {
        old->deliver(DEF_PROT_KICKED_OFFLINE, "");
        old->closeAfterWrite();
        log("[业务] 用户 id=", userId, " 在别处登录，旧连接被踢");
    }

    sendFriendInfo(s, userId, STATUS_ONLINE);
    for (const auto& f : m_server.db().getFriends(userId)) {
        const bool online = m_server.presence().isOnline(f.id);
        sendFriendInfo(s, f.id, online ? STATUS_ONLINE : STATUS_OFFLINE);
        if (auto fs = m_server.presence().get(f.id)) sendFriendInfo(fs, userId, STATUS_ONLINE);
    }

    auto undelivered = m_server.db().pullUndelivered(userId);
    std::vector<std::string> deliveredIds;
    for (const auto& m : undelivered) {
        ChatInfoRq out;
        if (!fillChatInfo(out, m, true)) continue;
        s->deliver(DEF_PROT_CHAT_INFO_RQ, out.SerializeAsString());
        deliveredIds.push_back(m.msgId);
        if (auto sender = m_server.presence().get(m.senderId)) {
            ChatInfoRs receipt;
            receipt.set_myid(userId);
            receipt.set_friid(m.senderId);
            receipt.set_result(CHAT_RESULT_SUCC);
            receipt.set_msg_id(m.msgId);
            receipt.set_seq(m.seq);
            sender->deliver(DEF_PROT_CHAT_INFO_RS, receipt.SerializeAsString());
        }
    }
    m_server.db().markDelivered(deliveredIds);
    if (!undelivered.empty()) log("[业务] 离线消息补发 id=", userId, " 条数=", undelivered.size());
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

    if (rq.device_id().empty() || rq.device_id().size() > 128) {
        LoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        s->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        return;
    }

    //一个TLS/TCP Session只能绑定一个账号，切换账号必须断开并建立连接
    if (s->authenticated()) {
    LoginRs rs;
    rs.set_result(LOGIN_PASSERROR);
    s->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());

    log(
        "[认证] 拒绝同一连接重复登录 currentUser=",
        s->userId()
    );
    return;
}

    TokenPair tokens;
    try {
        tokens = m_server.tokenService().issue(userId, rq.device_id());
    } catch (const std::exception& e) {
        log("[认证] Token 签发失败 id=", userId, " error=", e.what());
        LoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        s->deliver(DEF_PROT_LOGIN_RS, rs.SerializeAsString());
        return;
    }

    s->bindAuth(
        userId,
        tokens.sessionId,
        rq.device_id(),
        tokens.accessExpiresAt
    );

    LoginRs rs;
    rs.set_userid(userId);
    rs.set_result(LOGIN_SUCCESS);
    rs.set_access_token(
        tokens.accessToken
    );
    rs.set_refresh_token(
        tokens.refreshToken
    );
    rs.set_access_token_expire_at(
        tokens.accessExpiresAt
    );
    rs.set_refresh_token_expire_at(
        tokens.refreshExpiresAt
    );
    rs.set_session_id(
        tokens.sessionId
    );

    s->deliver(
        DEF_PROT_LOGIN_RS,
        rs.SerializeAsString()
    );

    activateAuthenticatedSession(s, userId);
    log("[业务] 登录成功 id=", userId);
}

void Dispatcher::onTokenLoginRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    TokenLoginRq rq;
    if (!parsePayload(payload, rq)) return;
    int userId = 0;
    std::string sessionId;
    std::int64_t expiresAt = 0;

    /*
    撤销当前认证会话 → 清除本地 Token → 断开 TLS → 创建新 TLS 连接 → 登录新账号
    */
    if (s->authenticated()) {
        TokenLoginRs rs;
        rs.set_result(LOGIN_PASSERROR);
        s->deliver(DEF_PROT_TOKEN_LOGIN_RS, rs.SerializeAsString());
        return;
    }

    const bool ok = rq.device_id().size() <= 128 &&
        m_server.tokenService().validateAccess(
            rq.access_token(), "", rq.device_id(), userId, sessionId, expiresAt);
    TokenLoginRs rs;
    rs.set_result(ok ? LOGIN_SUCCESS : LOGIN_PASSERROR);
    if (ok) {
        rs.set_userid(userId);
        rs.set_access_token_expire_at(expiresAt);
        s->bindAuth(userId, sessionId, rq.device_id(), expiresAt);
    }
    s->deliver(DEF_PROT_TOKEN_LOGIN_RS, rs.SerializeAsString());
    if (ok) activateAuthenticatedSession(s, userId);
}

void Dispatcher::onTokenRefreshRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    RefreshTokenRq rq;
    if (!parsePayload(payload, rq)) return;
    TokenPair tokens;
    int revokedUserId = 0;
    const bool ok = rq.device_id().size() <= 128 && rq.request_id().size() <= 128 &&
        m_server.tokenService().rotateRefresh(
            rq.refresh_token(), rq.device_id(), rq.request_id(), tokens, revokedUserId);
    // 检测到 refresh_token 重放：整个 token 家族已在数据库层面被吊销，但如果这个家族
    // 里有一个连接正在线（很可能就是被盗用的合法会话），它不会自己感知到被吊销，
    // 需要立即踢下线，不能只靠它 15 分钟后自然过期
    if (revokedUserId > 0) {
        if (auto live = m_server.presence().get(revokedUserId)) {
            live->deliver(DEF_PROT_KICKED_OFFLINE, "");
            live->closeAfterWrite();
            log("[认证] 检测到 refresh_token 重放，家族已吊销，踢下线 uid=", revokedUserId);
        }
    }
    RefreshTokenRs rs;
    rs.set_result(ok ? 0 : 1);
    if (ok) {
        rs.set_access_token(tokens.accessToken);
        rs.set_refresh_token(tokens.refreshToken);
        rs.set_access_token_expire_at(tokens.accessExpiresAt);
        rs.set_refresh_token_expire_at(tokens.refreshExpiresAt);
        rs.set_session_id(tokens.sessionId);
        // 关键：不只是把新 token 发给客户端，这条连接自己缓存的过期时间也要跟着更新，
        // 否则刷新明明成功了，dispatchPacket 的过期检查还是按旧的到期时间把连接断掉
        if (s->authenticated()) {
            s->bindAuth(s->userId(), tokens.sessionId, rq.device_id(), tokens.accessExpiresAt);
        }
    }
    s->deliver(DEF_PROT_TOKEN_REFRESH_RS, rs.SerializeAsString());
}

void Dispatcher::onLogoutRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    LogoutRq rq;
    if (!parsePayload(payload, rq)) return;
    Database::AuthSessionRecord record;
    /*注销 token完整性验证*/
    const bool tokenValid =!rq.refresh_token().empty() &&
        m_server.db().findByRefreshHash(
            TokenService::tokenHash(rq.refresh_token()),
            record
        ) &&
        !record.revoked &&
        record.deviceId == rq.device_id();

    /*注销 验证当前连接就是Token所属连接*/
    const bool ownsSession =
        tokenValid &&
        s->authenticated() &&
        s->userId() == record.userId &&
        s->deviceId() == record.deviceId &&
        s->authSessionId() == record.sessionId;

    const bool ok = ownsSession;
    log("[业务] 登出 id=", s->userId(), " 结果=", ok);

    if (ok) {
        if (rq.logout_all_devices()) 
            m_server.tokenService().revokeAllForUser(record.userId);
        else 
            m_server.tokenService().revokeSession(record.sessionId);
    }
    LogoutRs rs;
    rs.set_result(ok ? 0 : 1);
    s->deliver(DEF_PROT_LOGOUT_RS, rs.SerializeAsString());
    if (ok) s->closeAfterWrite(); // 只有真正验证通过、确实注销了这个连接自己的会话，才关闭它；
                                  // 验证失败时这条连接依然合法在线，不应该被误关
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

    const int userId = s->userId();     //以登录态为准
    if(userId <= 0) return;             //拒绝未登录链接
    rq.set_myid(userId);                //用服务端权威身份覆盖客户端自报的myid

    //判断friid是否是好友
    if (!m_server.db().isFriend(userId, rq.friid())) {
        ChatInfoRs rs;
        rs.set_myid(rq.friid());
        rs.set_friid(rq.myid());
        rs.set_msg_id(rq.msg_id());
        rs.set_result(CHAT_RESULT_NOT_FRIEND);
        s->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
        return;
    }

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

    auto target = m_server.presence().get(rq.friid());
    const bool delivered = static_cast<bool>(target);

    ChatInfoRs rs;
    rs.set_myid(rq.friid());
    rs.set_friid(rq.myid());
    // 统一落库（注意：saveMessage 需要负责为 m.seq 赋值）
    if(!m_server.db().saveMessage(m,delivered)){
        rs.set_msg_id(rq.msg_id());
        rs.set_result(CHAT_RESULT_SERVER_ERROR);
        rs.set_seq(0);
        s->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());

        log("[业务] 保存消息失败 msg_id=", rq.msg_id());
        return;
    }

    // 更新转发请求的数据
    rq.set_msg_id(m.msgId); 
    rq.set_ts(m.ts);
    rq.set_seq(m.seq);

    if (target) {
        // 在线：存历史（已投递）+ 转发（含图片字节）。补上服务端权威时间 ts + 会话 seq，
        target->deliver(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString());
        rs.set_msg_id(m.msgId); // 返回实际落库的 msgId
        rs.set_result(CHAT_RESULT_SUCC);
    } else {
        // 离线无需额外落库，前面 saveMessage(m, false) 已完成
        rs.set_msg_id(m.msgId); // 同样要带回实际落库的 msgId，否则发送方回执对不上本地待确认消息
        rs.set_result(CHAT_RESULT_FAIL);
    }
    rs.set_seq(m.seq); // 回执带回服务端分配的会话 seq，发送方据此校正本地消息顺序
    s->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
}

// ---------------- 下线 ----------------

void Dispatcher::onOfflineRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    FriendOffline pkt;
    if (!parsePayload(payload, pkt)) return;

    // 摘映射（onSessionClosed 会广播好友下线，这里只需关连接）。
    // 用 session 登录态而非 payload 的 offlineid：防止伪造 id 摘除他人在线映射。
    m_server.presence().offline(s->userId(), s);
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

    //身份鉴权，防身份伪造
    const int userId = s->userId();
    if(userId <= 0) return;
    rq.set_myid(userId);

    const int friendId = m_server.db().getUserIdByNick(utf8Truncate(rq.frinick(), USER_NICK_LEN - 1));
    if (friendId == 0) {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_NOTEXIT);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[业务] 添加好友失败，好友不存在 nick=", rq.frinick());
        return;
    }

    //如果是自己加自己也要拒绝
    if (userId == friendId) {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_SELF);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[业务] 添加好友失败，不能添加自己 nick=", rq.frinick());
        return;
    }

    //判断是否已经是好友
    if (m_server.db().isFriend(userId, friendId)) {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_ALREADY);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[业务] 添加好友失败，已经是好友 nick=", rq.frinick());
        return;
    }

    if (auto target = m_server.presence().get(friendId)) {
        {
            std::lock_guard<std::mutex> lg(m_pendingFriendMtx);
            m_pendingFriendReq.insert({userId, friendId}); // userId 已是登录态纠正后的发起人
        }
        target->deliver(DEF_PROT_ADD_FRIEND_RQ, rq.SerializeAsString()); // 在线则转发
        log("[业务] 添加好友请求 dest=", friendId);
    } else {
        AddFriendRs rs;
        rs.set_result(ADD_FRIEND_OFFLINE);
        rs.set_mynick(rq.frinick());
        s->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[业务] 添加好友失败，好友离线 nick=", rq.frinick());
    }
}

void Dispatcher::onAddFriendRs(const std::shared_ptr<Session>& s, const std::string& payload)
{
    AddFriendRs rs;
    if (!parsePayload(payload, rs)) return;

    //身份鉴权，防身份伪造
    const int userId = s->userId();
    if (userId <= 0) return;
    rs.set_myid(userId);

    // 必须确实存在一条"我方转发过"的请求记录才能消费；无论同意/拒绝都要 erase 掉，
    // 避免记录残留被重放（比如先拒绝一次，同一条记录之后又被拿去伪造同意）
    bool hasPendingReq;
    {
        std::lock_guard<std::mutex> lg(m_pendingFriendMtx);
        hasPendingReq = m_pendingFriendReq.erase({rs.destid(), userId}) > 0;
    }

    if (rs.result() == ADD_FRIEND_AGREE) {
        // 防止在对方从未发起请求的情况下伪造同意、强行建立好友关系
        // （进而绕过 onChatRq/onFileOfferRq 的 isFriend 校验网关）
        if (!hasPendingReq) {
            log("[业务] 添加好友同意被拒绝(无对应请求记录) destid=", rs.destid(), " myid=", userId);
            return;
        }
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
        dest->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString()); // 用纠正后的 rs，不是原始 payload
        log("[业务] 添加好友回复 dest=", rs.destid(), " result=", rs.result());
    }
}

// ---------------- 消息漫游（M6） ----------------

void Dispatcher::onRoamConvRq(const std::shared_ptr<Session>& s, const std::string&)
{
    const int userId = s->userId(); // 以登录态为准，忽略 payload myid
    if (userId <= 0) return;

    RoamConvRs rs;
    for (const auto& m : m_server.db().roamConversations(userId)) {
        // 会话列表预览：图片不读盘、不带字节（withImage=false），客户端显示"[图片]"
        fillChatInfo(*rs.add_convs(), m, /*withImage=*/false);
    }
    s->deliver(DEF_PROT_ROAM_CONV_RS, rs.SerializeAsString());
    log("[业务] 漫游会话列表 id=", userId, " 会话数=", rs.convs_size());
}

void Dispatcher::onRoamMsgRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    const int userId = s->userId(); // 以登录态为准，忽略 payload myid
    if (userId <= 0) return;

    RoamMsgRq rq;
    if (!parsePayload(payload, rq)) return;
    const int peerId = rq.peer_id();
    const std::int64_t beforeSeq = rq.before_seq() > 0 ? rq.before_seq() : INT64_MAX;
    int limit = rq.limit() > 0 ? rq.limit() : 20;
    if (limit > 100) limit = 100; // 上限保护

    auto rows = m_server.db().roamMessages(userId, peerId, beforeSeq, limit);

    RoamMsgRs rs;
    rs.set_peer_id(peerId);
    rs.set_has_more(static_cast<int>(rows.size()) == limit); // 满 N 条即认为还有更早的
    for (const auto& m : rows) {
        // 图片读盘回传完整字节；文件丢失则跳过该条（但不影响下面的游标推进）
        im::proto::ChatInfoRq tmp;
        if (!fillChatInfo(tmp, m, /*withImage=*/true)) continue;
        *rs.add_msgs() = std::move(tmp);
    }
    // 游标取本批 DB 原始最小 seq（rows 按 seq DESC，末条最小）。即使整页图片文件丢失被跳过，
    // 游标也随之前进，避免客户端上拉时卡在同一页反复空拉。
    rs.set_min_seq(rows.empty() ? 0 : rows.back().seq);
    s->deliver(DEF_PROT_ROAM_MSG_RS, rs.SerializeAsString());
    log("[业务] 漫游历史 id=", userId, " peer=", peerId,
        " before_seq=", beforeSeq, " 返回=", rs.msgs_size(), " hasMore=", rs.has_more());
}

std::string Dispatcher::filePartPath(const std::string& fileId) const
{
    return m_server.uploadDir() + "/file/tmp/" + fileId + ".part";
}
std::string Dispatcher::fileFinalPath(const std::string& fileId, const std::string& name) const
{
    // I2: 磁盘路径只用 basename，防止 file_name 携带路径穿越（../、绝对路径等）
    const std::string base = std::filesystem::path(name).filename().string();
    return m_server.uploadDir() + "/file/" + fileId + "_" + base;
}

namespace {
// I2: file_id（== msg_id）只允许安全字符集，防止用作路径片段时发生穿越
bool isSafeFileId(const std::string& id)
{
    if (id.empty()) return false;
    for (char c : id) {
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    }
    if (id.find("..") != std::string::npos) return false;
    return true;
}

bool isSha256Hex(const std::string& value)
{
    if (value.size() != 64) return false;
    for (unsigned char c : value) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}
} // namespace

void Dispatcher::onFileOfferRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileOfferRq rq;
    if (!parsePayload(payload, rq)) return;

    FileOfferRs rs;
    rs.set_msg_id(rq.msg_id());
    rs.set_file_id(rq.msg_id()); // file_id == msg_id
    const auto expectedChunks = static_cast<int>(
        (rq.file_size() + static_cast<std::int64_t>(FILE_CHUNK_SIZE) - 1) /
        static_cast<std::int64_t>(FILE_CHUNK_SIZE));
    const std::string safeName = std::filesystem::path(rq.file_name()).filename().string();
    if (rq.file_size() <= 0 || rq.file_size() > FILE_MAX_SIZE ||
        rq.total_chunks() != expectedChunks || !isSha256Hex(rq.sha256()) ||
        safeName.empty() || safeName.size() > 255) {
        rs.set_result(FILE_OFFER_TOO_LARGE);
        rs.set_received_chunks(0);
        s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
        log("[业务] 文件协商拒绝(非法大小) id=", s->userId(), " size=", rq.file_size());
        return;
    }
    if (!isSafeFileId(rq.msg_id())) {
        rs.set_result(FILE_OFFER_TOO_LARGE);
        rs.set_received_chunks(0);
        s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
        log("[业务] 文件协商拒绝(非法file_id) id=", s->userId(), " file_id=", rq.msg_id());
        return;
    }
    if(!m_server.db().isFriend(s->userId(), rq.receiver_id())){
        rs.set_result(FILE_OFFER_NOT_FRIEND);
        rs.set_received_chunks(0);
        s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_server.uploadDir() + "/file/tmp", ec);
    // 水位线 N = 已有 .part 大小 / CHUNK_SIZE（顺序追加，故整除即已连续收到块数）
    // I1: 末块可能是不足一个 CHUNK 的部分块，此时 partSize == fileSize 但非 CHUNK 整数倍，
    // 需要特判为"已全部收到"，否则 N 会回退指向最后一个块下标，导致重发死锁。
    const std::string part = filePartPath(rq.msg_id());
    std::int64_t partSize = 0;
    if (std::filesystem::exists(part, ec)) 
        partSize = (std::int64_t)std::filesystem::file_size(part, ec);
    int n;
    if (rq.file_size() > 0 && partSize >= rq.file_size()) {
        n = rq.total_chunks();
    } else {
        n = (int)(partSize / (std::int64_t)FILE_CHUNK_SIZE);
    }
    rs.set_received_chunks(n);
    rs.set_result(FILE_OFFER_OK);
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        auto it = m_pendingUploads.find(rq.msg_id());
        // 同 fileId 的断点续传只能由原上传者、以完全相同的元数据恢复。
        if (it != m_pendingUploads.end() &&
            (it->second.senderId != s->userId() || it->second.receiverId != rq.receiver_id() ||
             it->second.fileSize != rq.file_size() || it->second.sha256 != rq.sha256())) {
            rs.set_result(FILE_OFFER_TOO_LARGE); // 现有协议没有 CONFLICT，暂用通用拒绝码
            rs.set_received_chunks(0);
            s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
            return;
        }
        m_pendingUploads[rq.msg_id()] = PendingUpload{
            s->userId(), rq.receiver_id(), safeName, rq.file_size(), rq.total_chunks(), rq.sha256() };
    }
    s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
    log("[业务] 文件协商 id=", s->userId(), " file=", rq.file_name(), " size=", rq.file_size(), " N=", n);
}

void Dispatcher::onFileChunkRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileChunkRq rq;
    if (!parsePayload(payload, rq)) return;

    //文件id安全校验
    if(!isSafeFileId(rq.file_id())) return;

    // 文件 id 必须对应一次真实存在的 pendingUploads 记录，否则直接拒绝
    PendingUpload up;
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        auto it = m_pendingUploads.find(rq.file_id());
        if (it == m_pendingUploads.end()) return; // 查不到直接拒绝
        if (it->second.senderId != s->userId()) return; // 不能向别人的上传任务注入分片
        up = it->second;
    }

    const std::string part = filePartPath(rq.file_id());
    std::error_code ec;
    std::int64_t partSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;
    const int n = (int)(partSize / (std::int64_t)FILE_CHUNK_SIZE);
    // I1: 只有当仍有数据缺口（partSize < fileSize）时才追加，避免末块（不足一个 CHUNK）
    // 写入后被重复追加，导致 partSize 超过 fileSize 而使 onFileCompleteRq 永久失败。
    const bool stillExpectingData = (partSize < up.fileSize);
    const auto remaining = up.fileSize - partSize;
    const auto expectedChunkSize = static_cast<std::size_t>(
        std::min<std::int64_t>(FILE_CHUNK_SIZE, remaining));
    if (rq.chunk_index() == n && stillExpectingData && rq.data().size() == expectedChunkSize) {
        std::ofstream ofs(part, std::ios::binary | std::ios::app);
        ofs.write(rq.data().data(), (std::streamsize)rq.data().size());
    } // <n 幂等丢弃；>n 忽略（顺序传输不应乱序）；已收满则忽略（幂等）

    // 回进度（新水位线）
    std::int64_t newSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;
    FileProgressRs pr;
    pr.set_file_id(rq.file_id());
    pr.set_received_chunks((int)(newSize / (std::int64_t)FILE_CHUNK_SIZE));
    pr.set_status(FILE_ST_UPLOADING);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
}

void Dispatcher::onFileCompleteRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    const int uid = s->userId();
    if (uid <= 0) return;
    FileCompleteRq rq;
    if (!parsePayload(payload, rq)) return;
    if (!isSafeFileId(rq.file_id()) || rq.msg_id() != rq.file_id()) return;

    // 从 Offer 缓存不到元数据 —— Complete 只带 file_id/msg_id，需要文件名/size/sha/receiver。
    // 方案：Offer 时把元数据落一张内存表 m_pendingUploads[file_id]。
    PendingUpload up;
    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        auto it = m_pendingUploads.find(rq.file_id());
        if (it == m_pendingUploads.end()) return;
        if (it->second.senderId != uid) return; // 只能由创建 Offer 的账号完成
        up = it->second;
    }

    const std::string part = filePartPath(rq.file_id());
    std::error_code ec;
    std::int64_t partSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;

    auto fail = [&]() {
        std::filesystem::remove(part, ec);
        {
            std::lock_guard<std::mutex> lg(m_uploadMtx);
            m_pendingUploads.erase(rq.file_id());
        }
        FileProgressRs pr; 
        pr.set_file_id(rq.file_id());
        pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
    };

    if (partSize != up.fileSize) { fail(); return; }
    // 流式 sha256（复用 im::Sha256 增量类）
    im::Sha256 h;
    {
        std::ifstream ifs(part, std::ios::binary);
        std::vector<char> buf(64 * 1024);
        while (ifs) { ifs.read(buf.data(), (std::streamsize)buf.size());
            if (ifs.gcount() > 0) h.update(buf.data(), (std::size_t)ifs.gcount()); }
    }
    const std::vector<unsigned char> dg = h.final();
    static const char* hex = "0123456789abcdef";
    std::string got; got.reserve(64);
    for (unsigned char b : dg) { got.push_back(hex[(b>>4)&0xF]); got.push_back(hex[b&0xF]); }
    if (got != up.sha256) { fail(); return; }

    // 落盘：移动到成品
    const std::string finalPath = fileFinalPath(rq.file_id(), up.fileName);
    std::filesystem::rename(part, finalPath, ec);
    if (ec) { std::filesystem::copy_file(part, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
              std::filesystem::remove(part, ec); }

    // 建文件消息（type=2）
    StoredMessage m;
    m.msgId = rq.msg_id();
    m.senderId = uid;
    m.receiverId = up.receiverId;
    m.type = 2;
    m.content = up.fileName;       // content 存文件名
    m.mediaPath = finalPath;
    m.fileId = rq.file_id();
    m.fileSize = up.fileSize;
    m.ts = nowSec();
    // 只查询一次，保证写库的投递状态、实际转发对象和发送方回执口径一致。
    auto target = m_server.presence().get(up.receiverId);
    const bool online = (bool)target;
    if (!m_server.db().saveMessage(m, online)) {
        std::filesystem::remove(finalPath, ec);
        fail();
        return;
    }

    // 组文件卡片（ChatInfoRq type=FILE），转发在线接收方 / 离线等补发
    ChatInfoRq card;
    card.set_myid(uid);
    card.set_friid(up.receiverId);
    card.set_type(im::proto::FILE);
    card.set_msg_id(rq.msg_id());
    card.set_ts(m.ts);
    card.set_seq(m.seq);
    card.set_file_name(up.fileName);
    card.set_file_size(up.fileSize);
    card.set_file_id(rq.file_id());
    if (target) {
        target->deliver(DEF_PROT_CHAT_INFO_RQ, card.SerializeAsString());
    }

    {
        std::lock_guard<std::mutex> lg(m_uploadMtx);
        m_pendingUploads.erase(rq.file_id());
    }
    FileProgressRs pr; pr.set_file_id(rq.file_id());
    pr.set_status(FILE_ST_DONE);
    pr.set_received_chunks(up.totalChunks); pr.set_total_chunks(up.totalChunks);
    pr.set_seq(m.seq);
    pr.set_delivered(online);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
    log("[业务] 文件完成 file=", up.fileName, " -> ", finalPath, " online=", online);
}

void Dispatcher::onFileDownloadRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileDownloadRq rq;
    if (!parsePayload(payload, rq)) return;

    StoredMessage m;
    if (!m_server.db().getMessageByMsgId(rq.file_id(), m) || m.type != 2 || m.mediaPath.empty()) {
        FileProgressRs pr; 
        pr.set_file_id(rq.file_id()); 
        pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    const int uid = s->userId();
    if (uid != m.senderId && uid != m.receiverId) {
        // fileId 不是访问凭证；必须是这条文件消息的参与者。
        FileProgressRs pr; 
        pr.set_file_id(rq.file_id()); 
        pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    std::ifstream ifs(m.mediaPath, std::ios::binary);
    if (!ifs) {
        FileProgressRs pr; pr.set_file_id(rq.file_id()); pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    const int total = (int)((m.fileSize + (std::int64_t)FILE_CHUNK_SIZE - 1) / (std::int64_t)FILE_CHUNK_SIZE);
    int idx = rq.from_chunk() < 0 ? 0 : rq.from_chunk();
    if (idx > total) {
        FileProgressRs pr; pr.set_file_id(rq.file_id()); pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    ifs.seekg((std::streamoff)idx * (std::streamoff)FILE_CHUNK_SIZE, std::ios::beg);
    std::vector<char> buf(FILE_CHUNK_SIZE);
    for (; idx < total; ++idx) {
        ifs.read(buf.data(), (std::streamsize)FILE_CHUNK_SIZE);
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;
        FileChunkRq ch;
        ch.set_file_id(rq.file_id());
        ch.set_chunk_index(idx);
        ch.set_data(std::string(buf.data(), (size_t)got));
        s->deliver(DEF_PROT_FILE_CHUNK_RQ, ch.SerializeAsString());
    }
    FileProgressRs pr; pr.set_file_id(rq.file_id());
    pr.set_status(FILE_ST_DONE); pr.set_total_chunks(total); pr.set_received_chunks(total);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
}

} // namespace imsrv
