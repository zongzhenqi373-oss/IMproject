#pragma once
// client_core 对外主接口：连接管理 + 协议收发 + 业务逻辑 + 本地存储
// 设计对应原 Qt 客户端 kernal 类，但完全剥离 UI：
//   - 所有 UI 通知通过 IClientEvents 回调接口上抛（UI 层自行实现并切到 UI 线程）
//   - 所有 UI 请求通过 ClientCore 公开方法下发
// 线程模型：内部持有 asio 网络线程，IClientEvents 回调均在该线程触发；
//           UI 层（Qt/Electron/移动端）需自行 marshal 到 UI 线程。

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "Protocol.h"
#include "Types.h"

namespace im {

class IStorage;
class TcpTransport;

// UI 事件回调接口（由 UI 层实现）
class IClientEvents {
public:
    virtual ~IClientEvents() = default;

    // 注册结果：proto::REGISTER_SUCC / REGISTER_NICK_EXIT / REGISTER_TEL_EXIT
    virtual void onRegisterResult(int result) = 0;

    // 登录结果：proto::LOGIN_SUCCESS / LOGIN_NOTEXIT / LOGIN_PASSERROR
    virtual void onLoginResult(int result, int userId) = 0;

    // 自己的资料（登录成功后服务端下发）
    virtual void onSelfInfo(const UserInfo& info) = 0;

    // 好友资料（逐条下发，含在线状态）
    virtual void onFriendInfo(const FriendInfo& info) = 0;

    // 收到聊天消息（含离线补发），msg 为 UTF-8
    virtual void onChatMessage(int fromId, const std::string& msgUtf8) = 0;

    // 收到图片消息（含离线补发）：imageBytes 为压缩后图片字节，w/h 为尺寸
    virtual void onImageMessage(int fromId, const std::string& imageBytes,
                                int w, int h, const std::string& msgId) = 0;

    // 聊天发送结果：proto::CHAT_RESULT_SUCC（已送达）/ CHAT_RESULT_FAIL（对方离线已转存）
    virtual void onChatSendResult(int friId, int result) = 0;

    // 收到添加好友请求：需 UI 确认后调用 answerAddFriend()
    virtual void onAddFriendRequest(int fromId, const std::string& fromNickUtf8) = 0;

    // 添加好友结果：proto::ADD_FRIEND_AGREE / REJECT / OFFLINE / NOTEXIT
    virtual void onAddFriendResult(int result, const std::string& destNickUtf8) = 0;

    // 好友下线
    virtual void onFriendOffline(int userId) = 0;

    // 账号在别处登录被踢下线（对齐 QQNT session listener）
    // reason 保留扩展（当前恒为 0，表示"重复登录"）
    virtual void onKickedOffline(int reason) = 0;

    // 与服务端连接断开（对端关闭/网络异常/本地 close）
    virtual void onConnectionClosed() = 0;

    // ---------------- 消息漫游（M6，默认空实现，UI 按需覆盖） ----------------
    // 会话列表漫游结果：每会话最后一条（图片条目 imageBytes 为空，仅预览用）
    virtual void onRoamConversations(const std::vector<RoamMessage>& convs) { (void)convs; }
    // 会话历史分页结果：msgs 为本批（服务端 seq 倒序），hasMore/minSeq 供上拉翻页
    virtual void onRoamMessages(int peerId, const std::vector<RoamMessage>& msgs,
                                bool hasMore, std::int64_t minSeq)
    {
        (void)peerId; (void)msgs; (void)hasMore; (void)minSeq;
    }
};

class ClientCore {
public:
    ClientCore();
    ~ClientCore();

    // 禁止拷贝
    ClientCore(const ClientCore&) = delete;
    ClientCore& operator=(const ClientCore&) = delete;

    // 注入事件回调（非拥有指针，调用方保证生命周期长于 ClientCore）
    void setEventSink(IClientEvents* events);

    // 注入本地存储（可选，非拥有指针）；设置后自动持久化资料与聊天记录
    void setStorage(IStorage* storage);

    // 连接/断开服务端
    bool connectToServer(const std::string& ip, std::uint16_t port = proto::TCP_PORT);
    void disconnect();
    bool isConnected() const;

    // ---------------- 心跳保活 ----------------
    // 心跳间隔（毫秒，默认 30000）。连接成功后每间隔发一次 HEARTBEAT_RQ；
    // 连续 3 个间隔未收到服务端的任何数据则判定断连（触发 onConnectionClosed）。
    // 需在 connectToServer 之前调用才生效。
    void setHeartbeatIntervalMs(int intervalMs);

    // ---------------- 业务请求 ----------------
    void sendRegister(const std::string& nickUtf8, const std::string& tel, const std::string& pass);
    void sendLogin(const std::string& tel, const std::string& pass);
    void sendChatMessage(int friId, const std::string& msgUtf8);
    // 发送图片消息：imageBytes 为压缩后图片字节（建议 ≤500KB），w/h 为尺寸
    void sendImageMessage(int friId, const std::string& imageBytes, int w, int h);
    void sendAddFriendRequest(const std::string& friNickUtf8);
    // 回复添加好友请求：agree=true 同意，false 拒绝；destId/destNick 为请求发起人
    void answerAddFriend(int destId, const std::string& destNickUtf8, bool agree);
    // 通知服务端自己下线（下线后由调用方决定何时 disconnect）
    void sendOfflineNotify();

    // ---------------- 消息漫游（M6） ----------------
    // 登录后拉每会话最后一条（会话列表预览）
    void sendRoamConvRq();
    // 拉某会话比 beforeSeq 更早的 limit 条（首次传极大值拉最新，上拉传当前已加载最小 seq）
    void sendRoamMsgRq(int peerId, std::int64_t beforeSeq, int limit);

    // ---------------- 会话状态 ----------------
    int myId() const;
    std::string myNick() const;
    std::string myFeeling() const;
    int myIconId() const;

private:
    // 协议分发（沿用原 Kernel 函数指针数组设计）
    using DealFun = void (ClientCore::*)(const char* data, std::size_t len);
    DealFun m_dealFunArr[proto::DEF_PROT_COUNT]{};
    void initFunArr();
    void dispatchPacket(const char* data, std::size_t len);

    // 协议处理函数
    void onRegisterRs(const char* data, std::size_t len);
    void onLoginRs(const char* data, std::size_t len);
    void onFriendInfoPkt(const char* data, std::size_t len);
    void onChatInfoRq(const char* data, std::size_t len);
    void onChatInfoRs(const char* data, std::size_t len);
    void onAddFriRq(const char* data, std::size_t len);
    void onAddFriRs(const char* data, std::size_t len);
    void onFriendOfflinePkt(const char* data, std::size_t len);
    // 被踢下线通知
    void onKickedOfflinePkt(const char* data, std::size_t len);
    // 心跳回复：无需处理（任何入站包都会刷新活跃时间），注册避免"未注册类型"日志
    void onHeartbeatRs(const char* data, std::size_t len);
    // 漫游响应
    void onRoamConvRs(const char* data, std::size_t len);
    void onRoamMsgRs(const char* data, std::size_t len);

    // ---------------- 心跳保活 ----------------
    void startHeartbeat();
    void stopHeartbeat();

    std::thread m_hbThread;
    std::atomic<bool> m_hbRunning{false};
    int m_hbIntervalMs = 30000;
    // 最近一次收到服务端任意数据的时刻（steady_clock 毫秒），0 表示尚无
    std::atomic<std::int64_t> m_lastRecvMs{0};

    // 发送一个完整协议包：4B 小端协议号 + pb payload（transport 再加包长前缀）
    void sendPacket(proto::protType type, const std::string& payload);

    //跨线程指针：UI 线程设置（setEventSink/setStorage），asio io 线程与心跳线程读取
    std::atomic<IClientEvents*> m_events{nullptr};
    std::atomic<IStorage*> m_storage{nullptr};
    std::unique_ptr<TcpTransport> m_transport;

    // 会话状态（登录成功后填充）
    int m_myId = 0;
    int m_iconId = 0;
    std::string m_nick;
    std::string m_feeling;
};

} // namespace im
