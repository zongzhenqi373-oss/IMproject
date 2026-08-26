#pragma once
// 协议分发与业务处理器（对齐旧版 Kernel 的函数表分发模式）
// 所有 handler 在"会话业务 strand"上执行：同会话严格有序、跨会话并行。

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace imsrv {

class Server;
class Session;

class Dispatcher {
public:
    explicit Dispatcher(Server& server);

    void handle(const std::shared_ptr<Session>& s, std::uint32_t type, const std::string& payload);

private:
    using Handler = std::function<void(const std::shared_ptr<Session>&, const std::string&)>;

    void onRegisterRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onLoginRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onTokenLoginRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onTokenRefreshRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onLogoutRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onChatRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onOfflineRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onHeartbeatRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onAddFriendRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onAddFriendRs(const std::shared_ptr<Session>& s, const std::string& payload);
    void onRoamConvRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onRoamMsgRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileOfferRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileChunkRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileCompleteRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileDownloadRq(const std::shared_ptr<Session>& s, const std::string& payload);
    // 文件路径工具
    std::string filePartPath(const std::string& fileId) const;   // uploads/file/tmp/<id>.part
    std::string fileFinalPath(const std::string& fileId, const std::string& name) const; // uploads/file/<id>_<name>

    // 工具：下发一条 FriendInfo（在线状态由 Presence 决定）
    void sendFriendInfo(const std::shared_ptr<Session>& to, int userId, int onlineStatus);
    void activateAuthenticatedSession(const std::shared_ptr<Session>& s, int userId);
    // 图片落盘：uploads/img/<sha256>.<真实格式扩展名>（按魔数嗅探），返回相对路径
    std::string saveImage(const std::string& bytes);

    struct PendingUpload {
        int senderId = 0; // 上传任务必须绑定创建它的登录 Session，防止其他账号注入/完成分片
        int receiverId = 0;
        std::string fileName;
        std::int64_t fileSize = 0;
        int totalChunks = 0;
        std::string sha256;
    };
    std::unordered_map<std::string, PendingUpload> m_pendingUploads; // key=file_id
    std::mutex m_uploadMtx; // 保护 m_pendingUploads（跨会话业务 strand 并发访问）

    // 加好友：记录"确实转发过的请求" {requesterId, targetId}，onAddFriendRs 同意前据此核验，
    // 防止在没有真实请求的情况下伪造同意、强行建立好友关系（进而绕过 isFriend 校验网关）
    std::set<std::pair<int, int>> m_pendingFriendReq;
    std::mutex m_pendingFriendMtx;

    Server& m_server;
    std::unordered_map<std::uint32_t, Handler> m_handlers;
};

} // namespace imsrv
