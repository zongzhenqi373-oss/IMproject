#pragma once
// client_core 对外主接口：连接管理 + 协议收发 + 业务逻辑 + 本地存储
// 设计对应原 Qt 客户端 kernal 类，但完全剥离 UI：
//   - 所有 UI 通知通过 IClientEvents 回调接口上抛（UI 层自行实现并切到 UI 线程）
//   - 所有 UI 请求通过 ClientCore 公开方法下发
// 线程模型：内部持有 asio 网络线程，IClientEvents 回调均在该线程触发；
//           UI 层（Qt/Electron/移动端）需自行 marshal 到 UI 线程。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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

    // 聊天发送结果：proto::CHAT_RESULT_SUCC（已送达）/ CHAT_RESULT_FAIL（对方离线已转存）
    virtual void onChatSendResult(int friId, int result) = 0;

    // 收到添加好友请求：需 UI 确认后调用 answerAddFriend()
    virtual void onAddFriendRequest(int fromId, const std::string& fromNickUtf8) = 0;

    // 添加好友结果：proto::ADD_FRIEND_AGREE / REJECT / OFFLINE / NOTEXIT
    virtual void onAddFriendResult(int result, const std::string& destNickUtf8) = 0;

    // 好友下线
    virtual void onFriendOffline(int userId) = 0;

    // 与服务端连接断开（对端关闭/网络异常/本地 close）
    virtual void onConnectionClosed() = 0;
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

    // ---------------- 业务请求 ----------------
    void sendRegister(const std::string& nickUtf8, const std::string& tel, const std::string& pass);
    void sendLogin(const std::string& tel, const std::string& pass);
    void sendChatMessage(int friId, const std::string& msgUtf8);
    void sendAddFriendRequest(const std::string& friNickUtf8);
    // 回复添加好友请求：agree=true 同意，false 拒绝；destId/destNick 为请求发起人
    void answerAddFriend(int destId, const std::string& destNickUtf8, bool agree);
    // 通知服务端自己下线（下线后由调用方决定何时 disconnect）
    void sendOfflineNotify();

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

    // 发送一个完整协议包：4B 小端协议号 + pb payload（transport 再加包长前缀）
    void sendPacket(proto::protType type, const std::string& payload);

    IClientEvents* m_events = nullptr;
    IStorage* m_storage = nullptr;
    std::unique_ptr<TcpTransport> m_transport;

    // 会话状态（登录成功后填充）
    int m_myId = 0;
    int m_iconId = 0;
    std::string m_nick;
    std::string m_feeling;
};

} // namespace im
