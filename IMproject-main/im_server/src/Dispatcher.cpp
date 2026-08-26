#include "Dispatcher.h"
#include "Server.h"
#include "Session.h"
#include "Log.h"
#include "HttpFileServer.h"
#include "client_core/Protocol.h"
#include "im.pb.h"
#include "sha256.h"
#include "auth/TokenService.h"

#include <atomic>
#include <cstdint>
#include <ctime>
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
// 把一条 StoredMessage 填入 ChatInfoRq（漫游/补发共用口径：myid=发送方, friid=接收方, 带 ts/seq）。
// 图片/文件消息只回填元数据（file_id/file_name/file_size），不再读盘带字节——
// 接收方按需向 HTTP 文件服务发起下载。sha256/content_type 目前只在实时转发时由 onChatRq
// 现填（见下），漫游/补发这条暂不回填（StoredMessage 未持久化这两个字段，属已知简化，
// 不影响下载正确性，只是历史消息卡片上拿不到这两个展示用的辅助字段）。
bool fillChatInfo(im::proto::ChatInfoRq& out, const StoredMessage& m, bool /*withImage*/)
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
        out.set_file_id(m.fileId);
        out.set_file_size(m.fileSize);
        out.set_content_type(m.contentType);
        out.set_sha256(m.sha256);
    } else if (m.type == 2) {
        out.set_type(im::proto::FILE);
        out.set_file_id(m.fileId);
        out.set_file_name(m.content);
        out.set_file_size(m.fileSize);
        out.set_content_type(m.contentType);
        out.set_sha256(m.sha256);
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

    // 图片/文件已经通过 HTTP 文件服务上传完成，这里只核对 file_id 确实是当前发送者、
    // 发给同一个接收者上传的（一次性认领，防重放）——不再经手任何媒体字节。
    // 见 SECURITY_REVIEW.md 第 8/42 条：这一步是取代旧分片协议里"每个 chunk 各自校验身份"
    // 的收口点，一次鉴权、一次核验，杜绝 file_id 被盗用冒领。
    std::string mediaPath;
    std::int64_t fileSize = 0;
    if (rq.type() == IMAGE || rq.type() == im::proto::FILE) {
        HttpFileServer::UploadRecord record;
        if (!m_server.httpFileServer()->findUploadRecord(rq.file_id(), record) ||
            record.uploaderId != userId || record.receiverId != rq.friid()) {
            ChatInfoRs rs;
            rs.set_myid(rq.friid());
            rs.set_friid(rq.myid());
            rs.set_msg_id(rq.msg_id());
            rs.set_result(CHAT_RESULT_FILE_NOT_OWNED);
            s->deliver(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
            return;
        }
        mediaPath = record.mediaPath;
        fileSize = record.size;
        rq.set_file_size(fileSize);
        rq.set_content_type(record.contentType);
        rq.set_sha256(record.sha256);
    }

    StoredMessage m;
    m.msgId = rq.msg_id().empty() ? fallbackMsgId(rq.myid(), rq.friid(), rq.msg()) : rq.msg_id();
    m.senderId = rq.myid();
    m.receiverId = rq.friid();
    m.type = rq.type() == IMAGE ? 1 : (rq.type() == im::proto::FILE ? 2 : 0);
    m.content = rq.type() == im::proto::FILE ? rq.file_name() : rq.msg();
    m.mediaPath = mediaPath;
    m.imgW = rq.image_width();
    m.imgH = rq.image_height();
    m.fileId = rq.file_id();
    m.fileSize = fileSize;
    m.contentType = rq.content_type();
    m.sha256 = rq.sha256();
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
    if (m.type == 1 || m.type == 2) {
        m_server.httpFileServer()->eraseUploadRecord(m.fileId);
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
        im::proto::ChatInfoRq tmp;
        if (!fillChatInfo(tmp, m, /*withImage=*/true)) continue;
        *rs.add_msgs() = std::move(tmp);
    }
    // 游标取本批 DB 原始最小 seq（rows 按 seq DESC，末条最小）
    rs.set_min_seq(rows.empty() ? 0 : rows.back().seq);
    s->deliver(DEF_PROT_ROAM_MSG_RS, rs.SerializeAsString());
    log("[业务] 漫游历史 id=", userId, " peer=", peerId,
        " before_seq=", beforeSeq, " 返回=", rs.msgs_size(), " hasMore=", rs.has_more());
}

} // namespace imsrv
