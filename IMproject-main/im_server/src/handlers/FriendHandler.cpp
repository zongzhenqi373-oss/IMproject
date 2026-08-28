#include "handlers/FriendHandler.h"

#include "Database.h"
#include "Log.h"
#include "Presence.h"
#include "Session.h"
#include "client_core/Protocol.h"
#include "handlers/HandlerUtils.h"
#include "im.pb.h"

namespace imsrv {
using namespace im::proto;

FriendHandler::FriendHandler(Database& db, Presence& presence)
    : m_db(db), m_presence(presence)
{
}

void FriendHandler::onAddFriend(const std::shared_ptr<Session>& session,
                                const std::string& payload)
{
    AddFriendRq rq;
    if (!handlers::parsePayload(payload, rq)) return;
    const int userId = session->userId();
    if (userId <= 0) return;
    rq.set_myid(userId);

    const int friendId = m_db.getUserIdByNick(
        utf8Truncate(rq.frinick(), USER_NICK_LEN - 1));
    AddFriendRs error;
    error.set_mynick(rq.frinick());
    if (friendId == 0) {
        error.set_result(ADD_FRIEND_NOTEXIT);
    } else if (userId == friendId) {
        error.set_result(ADD_FRIEND_SELF);
    } else if (m_db.isFriend(userId, friendId)) {
        error.set_result(ADD_FRIEND_ALREADY);
    } else if (auto target = m_presence.get(friendId)) {
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingRequests.insert({userId, friendId});
        }
        target->deliver(DEF_PROT_ADD_FRIEND_RQ, rq.SerializeAsString());
        log("[好友] 请求 user=", userId, " target=", friendId);
        return;
    } else {
        error.set_result(ADD_FRIEND_OFFLINE);
    }
    session->deliver(DEF_PROT_ADD_FRIEND_RS, error.SerializeAsString());
}

void FriendHandler::onAddFriendReply(const std::shared_ptr<Session>& session,
                                     const std::string& payload)
{
    AddFriendRs rs;
    if (!handlers::parsePayload(payload, rs)) return;
    const int userId = session->userId();
    if (userId <= 0) return;
    rs.set_myid(userId);

    bool hasPendingRequest = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        hasPendingRequest = m_pendingRequests.erase({rs.destid(), userId}) > 0;
    }
    if (rs.result() == ADD_FRIEND_AGREE) {
        if (!hasPendingRequest) {
            log("[好友] 拒绝无对应申请的同意 requester=", rs.destid(), " target=", userId);
            return;
        }
        m_db.addFriendBidirectional(rs.destid(), userId);
        handlers::sendFriendInfo(m_db, session, userId, STATUS_ONLINE);
        for (const auto& friendRecord : m_db.getFriends(userId)) {
            handlers::sendFriendInfo(m_db, session, friendRecord.id,
                m_presence.isOnline(friendRecord.id) ? STATUS_ONLINE : STATUS_OFFLINE);
        }
        if (auto requester = m_presence.get(rs.destid())) {
            handlers::sendFriendInfo(m_db, requester, rs.destid(), STATUS_ONLINE);
            for (const auto& friendRecord : m_db.getFriends(rs.destid())) {
                handlers::sendFriendInfo(m_db, requester, friendRecord.id,
                    m_presence.isOnline(friendRecord.id) ? STATUS_ONLINE : STATUS_OFFLINE);
            }
        }
    }
    if (auto requester = m_presence.get(rs.destid())) {
        requester->deliver(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString());
        log("[好友] 回复 requester=", rs.destid(), " result=", rs.result());
    }
}

void FriendHandler::onDeleteFriend(const std::shared_ptr<Session> &session, const std::string &payload)
{
    DeleteFriendRq rq;
    if(!handlers::parsePayload(payload, rq)){
        return;
    }
    const int userId = session->userId();
    const int friendId = rq.friend_id();

    DeleteFriendRs rs;
    rs.set_friend_id(friendId);
    if (userId <= 0 || friendId <= 0 || userId == friendId) {
        rs.set_result(DELETE_FRIEND_INVALID);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    if(!m_db.isFriend(userId, friendId)) {
        rs.set_result(DELETE_FRIEND_NOT_FRIEND);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    if(!m_db.removeFriendBidirectional(userId, friendId)) {
        rs.set_result(DELETE_FRIEND_DB_ERROR);
        session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());
        return;
    }
    rs.set_result(DELETE_FRIEND_SUCCESS);
    session->deliver(DEF_PROT_DELETE_FRIEND_RS, rs.SerializeAsString());

    //对方在线也要立即从其好友列表移除当前用户
    if(auto friendSession = m_presence.get(friendId)) {
        DeleteFriendRs rs_notice;
        rs_notice.set_friend_id(userId);
        rs_notice.set_result(DELETE_FRIEND_SUCCESS);
        friendSession->deliver(DEF_PROT_DELETE_FRIEND_RS, rs_notice.SerializeAsString());
    }
    
    log("[好友] 删除 user=", userId, " friend=", friendId);
}

} // namespace imsrv
