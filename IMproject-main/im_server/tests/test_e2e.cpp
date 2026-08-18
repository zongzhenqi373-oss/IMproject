// im_server 端到端测试：真服务端（进程内）+ client_core 真客户端回环
// 覆盖 M1/M2：注册、登录（加盐哈希校验）、资料下发、在线文本转发、
//            离线消息补发、心跳保活、多端互踢

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Server.h"
#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"
#include "sha256.h"

#include <fstream>

using namespace im;
using namespace im::proto;

namespace {

constexpr std::uint16_t TEST_PORT = 24680;
const char* TEST_DB = "/tmp/im_server_e2e.db";
const char* TEST_UPLOADS = "/tmp/im_server_e2e_uploads";

struct RecordingEvents : IClientEvents {
    std::mutex mtx;
    std::condition_variable cv;

    int registerResult = -1;
    int loginResult = -1;
    int loginUserId = 0;
    UserInfo self;
    bool gotSelf = false;
    std::vector<im::FriendInfo> friends;
    std::vector<std::pair<int, std::string>> chats;
    std::vector<std::pair<int, int>> chatResults;
    std::vector<int> offlineEvents;
    int kicked = -1;
    bool closed = false;

    struct ImageMsg { int fromId; std::string bytes; int w; int h; std::string msgId; };
    std::vector<ImageMsg> images;

    void notify() { cv.notify_all(); }
    void onRegisterResult(int r) override { std::lock_guard<std::mutex> l(mtx); registerResult = r; notify(); }
    void onLoginResult(int r, int uid) override { std::lock_guard<std::mutex> l(mtx); loginResult = r; loginUserId = uid; notify(); }
    void onSelfInfo(const UserInfo& i) override { std::lock_guard<std::mutex> l(mtx); self = i; gotSelf = true; notify(); }
    void onFriendInfo(const im::FriendInfo& f) override { std::lock_guard<std::mutex> l(mtx); friends.push_back(f); notify(); }
    void onChatMessage(int from, const std::string& m) override { std::lock_guard<std::mutex> l(mtx); chats.emplace_back(from, m); notify(); }
    void onImageMessage(int from, const std::string& bytes, int w, int h, const std::string& msgId) override {
        std::lock_guard<std::mutex> l(mtx); images.push_back({from, bytes, w, h, msgId}); notify();
    }
    void onChatSendResult(int friId, int r) override { std::lock_guard<std::mutex> l(mtx); chatResults.emplace_back(friId, r); notify(); }
    void onAddFriendRequest(int, const std::string&) override {}
    void onAddFriendResult(int, const std::string&) override {}
    void onFriendOffline(int uid) override { std::lock_guard<std::mutex> l(mtx); offlineEvents.push_back(uid); notify(); }
    void onKickedOffline(int reason) override { std::lock_guard<std::mutex> l(mtx); kicked = reason; notify(); }
    void onConnectionClosed() override { std::lock_guard<std::mutex> l(mtx); closed = true; notify(); }

    template <typename Pred>
    bool waitFor(Pred pred, int timeoutMs = 5000)
    {
        std::unique_lock<std::mutex> l(mtx);
        return cv.wait_for(l, std::chrono::milliseconds(timeoutMs), pred);
    }
};

// 已知种子用户（seedIfEmpty 预置，密码均 123456）
struct SeedUser { const char* nick; const char* tel; };
const SeedUser kZhangsan{"张三", "13800000001"};
const SeedUser kLisi{"李四", "13800000002"};

} // namespace

int main()
{
    std::remove(TEST_DB);
    std::remove((std::string(TEST_DB) + "-wal").c_str());
    std::remove((std::string(TEST_DB) + "-shm").c_str());

    imsrv::Server server(TEST_PORT, 2, 2, TEST_DB, TEST_UPLOADS);
    assert(server.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等监听就绪

    // ============ M1：注册 + 登录 + 文本 ============

    // 注册新用户（密码会经 sha256 哈希后传输）
    RecordingEvents regEv;
    ClientCore regClient;
    regClient.setEventSink(&regEv);
    assert(regClient.connectToServer("127.0.0.1", TEST_PORT));
    regClient.sendRegister("新用户", "13900000001", "mypassword");
    assert(regEv.waitFor([&] { return regEv.registerResult == REGISTER_SUCC; }));
    // 重复注册同昵称 → 应返回昵称已存在
    regClient.sendRegister("新用户", "13900000002", "mypassword");
    assert(regEv.waitFor([&] { return regEv.registerResult == REGISTER_NICK_EXIT; }));
    regClient.disconnect();

    // A（张三）登录
    RecordingEvents ea;
    ClientCore a;
    a.setEventSink(&ea);
    a.setHeartbeatIntervalMs(500);
    assert(a.connectToServer("127.0.0.1", TEST_PORT));
    a.sendLogin(kZhangsan.tel, "123456");
    assert(ea.waitFor([&] { return ea.loginResult == LOGIN_SUCCESS; }));
    const int idA = ea.loginUserId;
    assert(ea.waitFor([&] { return ea.gotSelf; }));
    assert(ea.self.nick == "张三");

    // 密码错误应失败
    RecordingEvents badEv;
    ClientCore bad;
    bad.setEventSink(&badEv);
    assert(bad.connectToServer("127.0.0.1", TEST_PORT));
    bad.sendLogin(kZhangsan.tel, "wrongpass");
    assert(badEv.waitFor([&] { return badEv.loginResult == LOGIN_PASSERROR; }));
    bad.disconnect();

    // B（李四）登录，好友列表应含在线的张三
    RecordingEvents eb;
    ClientCore b;
    b.setEventSink(&eb);
    b.setHeartbeatIntervalMs(500);
    assert(b.connectToServer("127.0.0.1", TEST_PORT));
    b.sendLogin(kLisi.tel, "123456");
    assert(eb.waitFor([&] { return eb.loginResult == LOGIN_SUCCESS; }));
    const int idB = eb.loginUserId;
    assert(eb.waitFor([&] {
        for (const auto& f : eb.friends) if (f.id == idA && f.status == STATUS_ONLINE) return true;
        return false;
    }));

    // A → B 文本在线转发
    a.sendChatMessage(idB, "你好李四");
    assert(eb.waitFor([&] {
        for (const auto& c : eb.chats) if (c.first == idA && c.second == "你好李四") return true;
        return false;
    }));
    assert(ea.waitFor([&] {
        for (const auto& r : ea.chatResults) if (r.first == idB && r.second == CHAT_RESULT_SUCC) return true;
        return false;
    }));

    // ============ M2：心跳 / 离线补发 / 互踢 ============

    // 心跳保活：保持 ~1.5s（≈3 个心跳周期），连接应保持（服务端应答了心跳）
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    assert(a.isConnected());
    assert(b.isConnected());

    // B 断开（模拟下线）→ A 应收到 B 的离线通知
    b.disconnect();
    assert(ea.waitFor([&] {
        for (int uid : ea.offlineEvents) if (uid == idB) return true;
        return false;
    }));

    // A → B 离线消息 → B 重连后补发
    a.sendChatMessage(idB, "这是离线消息");
    RecordingEvents eb2;
    ClientCore b2;
    b2.setEventSink(&eb2);
    assert(b2.connectToServer("127.0.0.1", TEST_PORT));
    b2.sendLogin(kLisi.tel, "123456");
    assert(eb2.waitFor([&] { return eb2.loginResult == LOGIN_SUCCESS; }));
    assert(eb2.waitFor([&] {
        for (const auto& c : eb2.chats) if (c.first == idA && c.second == "这是离线消息") return true;
        return false;
    }));

    // 互踢：张三在第二个连接上再登录 → 旧连接 a 收到被踢
    RecordingEvents ea2;
    ClientCore a2;
    a2.setEventSink(&ea2);
    assert(a2.connectToServer("127.0.0.1", TEST_PORT));
    a2.sendLogin(kZhangsan.tel, "123456");
    assert(ea2.waitFor([&] { return ea2.loginResult == LOGIN_SUCCESS; }));
    assert(ea.waitFor([&] { return ea.kicked == 0; }));

    // ============ M3：图片消息（在线转发 + 服务端落盘 + 离线补发） ============

    // 构造伪 PNG 图片字节（真实 PNG 魔数开头，>1KB，含 0 字节，验证二进制安全）
    std::string imgBytes;
    for (int i = 0; i < 2048; ++i) imgBytes.push_back(static_cast<char>(i % 256));
    imgBytes[0] = '\x89'; imgBytes[1] = 'P'; imgBytes[2] = 'N'; imgBytes[3] = 'G';
    imgBytes[4] = '\x0D'; imgBytes[5] = '\x0A'; imgBytes[6] = '\x1A'; imgBytes[7] = '\x0A';

    // a2（张三）→ b2（李四）在线发图片
    a2.sendImageMessage(idB, imgBytes, 64, 64);
    assert(eb2.waitFor([&] {
        for (const auto& im : eb2.images)
            if (im.fromId == idA && im.bytes == imgBytes && im.w == 64 && im.h == 64) return true;
        return false;
    }));

    // 服务端应已按魔数嗅探落盘 uploads/img/<sha256>.png
    const std::string expectImg = std::string(TEST_UPLOADS) + "/img/" + im::sha256Hex(imgBytes) + ".png";
    {
        std::ifstream f(expectImg, std::ios::binary | std::ios::ate);
        assert(f && f.tellg() == (std::streamsize)imgBytes.size());
    }

    // 离线图片：b2 断开 → a2 发图 → b2 重连后应补发图片（读盘回传字节）
    b2.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 等服务端感知断开
    std::string imgBytes2(1024, '\x5A');
    a2.sendImageMessage(idB, imgBytes2, 32, 32);

    RecordingEvents eb3;
    ClientCore b3;
    b3.setEventSink(&eb3);
    assert(b3.connectToServer("127.0.0.1", TEST_PORT));
    b3.sendLogin(kLisi.tel, "123456");
    assert(eb3.waitFor([&] { return eb3.loginResult == LOGIN_SUCCESS; }));
    assert(eb3.waitFor([&] {
        for (const auto& im : eb3.images)
            if (im.fromId == idA && im.bytes == imgBytes2 && im.w == 32) return true;
        return false;
    }));

    // 送达回执：b3 上线收讫后，a2 应收到补发消息的 SUCC 回执（"已转存"→"已送达"）
    // （在线图片 SUCC 1 次 + 离线文本/图片补发回执 ≥1 次，这里断言总数 ≥2）
    assert(ea2.waitFor([&] {
        int succ = 0;
        for (const auto& cr : ea2.chatResults)
            if (cr.first == idB && cr.second == CHAT_RESULT_SUCC) ++succ;
        return succ >= 2;
    }));

    a2.disconnect();
    b3.disconnect();
    server.stop();

    std::cout << "test_e2e PASSED" << std::endl;
    return 0;
}
