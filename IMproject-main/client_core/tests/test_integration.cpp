// 回环集成测试：内嵌 asio 假服务端（线格式与真实服务端一致：
// [4B 大端包长][4B 小端协议号][pb payload]），
// 验证 ClientCore 的连接、登录、资料下发、聊天、离线消息、断连全链路。

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

using namespace im;
using namespace im::proto;
using asio::ip::tcp;

namespace {

// ---------------- 假服务端 ----------------
// 行为对齐 IMServer：登录成功 → 回 LoginRs → 下发自资料 → 下发好友 → 补发一条"离线消息"
class FakeServer {
public:
    bool start(std::uint16_t& outPort)
    {
        m_acceptor = std::make_unique<tcp::acceptor>(m_io, tcp::endpoint(tcp::v4(), 0));
        outPort = m_acceptor->local_endpoint().port();
        m_thread = std::thread([this]() { run(); });
        return true;
    }

    void stop()
    {
        m_io.stop();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void run()
    {
        tcp::socket sock(m_io);
        asio::error_code ec;
        m_acceptor->accept(sock, ec);
        if (ec) return;

        for (;;) {
            std::array<char, 4> hdr{};
            asio::read(sock, asio::buffer(hdr), ec);
            if (ec) return;
            const std::uint32_t len = decodeLen32(hdr.data());
            if (len < 4 || len > MAX_PACK_LEN) return;

            std::vector<char> body(len);
            asio::read(sock, asio::buffer(body), ec);
            if (ec) return;

            const protType type = decodeType32(body.data());
            if (!handle(type, body.data() + 4, len - 4, sock)) return;
        }
    }

    bool handle(protType type, const char* payload, std::size_t payloadLen, tcp::socket& sock)
    {
        switch (type) {
        case DEF_PROT_REGISTER_RQ: {
            im::proto::RegisterRs rs;
            rs.set_result(REGISTER_SUCC);
            return sendPkt(sock, DEF_PROT_REGISTER_RS, rs.SerializeAsString());
        }
        case DEF_PROT_LOGIN_RQ: {
            im::proto::LoginRs rs;
            rs.set_userid(42);
            rs.set_result(LOGIN_SUCCESS);
            if (!sendPkt(sock, DEF_PROT_LOGIN_RS, rs.SerializeAsString())) return false;

            // 自己资料
            im::proto::FriendInfo self;
            self.set_userid(42);
            self.set_iconid(3);
            self.set_status(STATUS_ONLINE);
            self.set_nick("测试用户");
            self.set_feeling("个性签名");
            if (!sendPkt(sock, DEF_PROT_FRIEND_INFO, self.SerializeAsString())) return false;

            // 好友资料
            im::proto::FriendInfo fri;
            fri.set_userid(7);
            fri.set_iconid(1);
            fri.set_status(STATUS_ONLINE);
            fri.set_nick("张三");
            fri.set_feeling("我是张三");
            if (!sendPkt(sock, DEF_PROT_FRIEND_INFO, fri.SerializeAsString())) return false;

            // 离线消息补发（张三 → 我）
            im::proto::ChatInfoRq offline;
            offline.set_myid(7);
            offline.set_friid(42);
            offline.set_msg("这是离线消息");
            return sendPkt(sock, DEF_PROT_CHAT_INFO_RQ, offline.SerializeAsString());
        }
        case DEF_PROT_CHAT_INFO_RQ: {
            im::proto::ChatInfoRq rq;
            if (!rq.ParseFromArray(payload, static_cast<int>(payloadLen))) return false;
            // 模拟对方在线：回复送达（myid=接收方, friid=发送方）
            im::proto::ChatInfoRs rs;
            rs.set_myid(rq.friid());
            rs.set_friid(rq.myid());
            rs.set_result(CHAT_RESULT_SUCC);
            return sendPkt(sock, DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString());
        }
        case DEF_PROT_FRIEND_OFFLINE:
            // 模拟服务端收到下线通知后关闭连接
            return false;
        default:
            return true;
        }
    }

    // 组帧：4B 大端包长 + 4B 小端协议号 + pb payload
    bool sendPkt(tcp::socket& sock, protType type, const std::string& payload)
    {
        const std::uint32_t bodyLen = static_cast<std::uint32_t>(4 + payload.size());
        std::vector<char> buf(4 + bodyLen);
        encodeLen32(bodyLen, buf.data());
        encodeType32(type, buf.data() + 4);
        std::memcpy(buf.data() + 8, payload.data(), payload.size());
        asio::error_code ec;
        asio::write(sock, asio::buffer(buf), ec);
        return !ec;
    }

    static void encodeLen32(std::uint32_t v, char* out)
    {
        out[0] = static_cast<char>((v >> 24) & 0xFF);
        out[1] = static_cast<char>((v >> 16) & 0xFF);
        out[2] = static_cast<char>((v >> 8) & 0xFF);
        out[3] = static_cast<char>(v & 0xFF);
    }
    static std::uint32_t decodeLen32(const char* p)
    {
        const auto* u = reinterpret_cast<const unsigned char*>(p);
        return (static_cast<std::uint32_t>(u[0]) << 24) |
               (static_cast<std::uint32_t>(u[1]) << 16) |
               (static_cast<std::uint32_t>(u[2]) << 8) |
               static_cast<std::uint32_t>(u[3]);
    }

    asio::io_context m_io;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::thread m_thread;
};

// ---------------- 记录型事件接收器 ----------------
struct RecordingEvents : IClientEvents {
    std::mutex mtx;
    std::condition_variable cv;

    int loginResult = -1;
    int loginUserId = 0;
    UserInfo self;
    bool gotSelf = false;
    std::vector<im::FriendInfo> friends; // 显式限定，避免与 pb 消息 proto::FriendInfo 冲突
    std::vector<std::pair<int, std::string>> chats;
    std::vector<std::pair<int, int>> chatResults;
    bool closed = false;

    void notify() { cv.notify_all(); }
    void onRegisterResult(int) override {}
    void onLoginResult(int result, int userId) override { std::lock_guard<std::mutex> l(mtx); loginResult = result; loginUserId = userId; notify(); }
    void onSelfInfo(const UserInfo& info) override { std::lock_guard<std::mutex> l(mtx); self = info; gotSelf = true; notify(); }
    void onFriendInfo(const im::FriendInfo& info) override { std::lock_guard<std::mutex> l(mtx); friends.push_back(info); notify(); }
    void onChatMessage(int fromId, const std::string& msg) override { std::lock_guard<std::mutex> l(mtx); chats.emplace_back(fromId, msg); notify(); }
    void onChatSendResult(int friId, int result) override { std::lock_guard<std::mutex> l(mtx); chatResults.emplace_back(friId, result); notify(); }
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int) override {}
    void onConnectionClosed() override { std::lock_guard<std::mutex> l(mtx); closed = true; notify(); }

    template <typename Pred>
    bool waitFor(Pred pred, int timeoutMs = 3000)
    {
        std::unique_lock<std::mutex> l(mtx);
        return cv.wait_for(l, std::chrono::milliseconds(timeoutMs), pred);
    }
};

} // namespace

int main()
{
    FakeServer server;
    std::uint16_t port = 0;
    assert(server.start(port));

    RecordingEvents ev;
    ClientCore core;
    core.setEventSink(&ev);
    assert(core.connectToServer("127.0.0.1", port));

    // 登录 → 应依次收到：登录回复 / 自资料 / 好友资料 / 离线消息
    core.sendLogin("13800000000", "123456");
    assert(ev.waitFor([&] { return ev.loginResult == LOGIN_SUCCESS; }));
    assert(ev.loginUserId == 42);
    assert(ev.waitFor([&] { return ev.gotSelf; }));
    assert(ev.self.nick == "测试用户");
    assert(ev.waitFor([&] { return !ev.friends.empty(); }));
    assert(ev.friends[0].id == 7 && ev.friends[0].nick == "张三");
    assert(ev.waitFor([&] { return !ev.chats.empty(); }));
    assert(ev.chats[0].first == 7 && ev.chats[0].second == "这是离线消息");

    // 会话状态应已填充
    assert(core.myId() == 42);
    assert(core.myNick() == "测试用户");

    // 发聊天 → 服务端回送达结果
    core.sendChatMessage(7, "你好张三");
    assert(ev.waitFor([&] { return !ev.chatResults.empty(); }));
    assert(ev.chatResults[0].first == 7 && ev.chatResults[0].second == CHAT_RESULT_SUCC);

    // 下线通知 → 假服务端关连接 → 客户端应收到断连回调
    core.sendOfflineNotify();
    assert(ev.waitFor([&] { return ev.closed; }));

    core.disconnect();
    server.stop();

    std::cout << "test_integration PASSED" << std::endl;
    return 0;
}
