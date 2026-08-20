#include "Server.h"
#include "Session.h"
#include "Dispatcher.h"
#include "Log.h"
#include "client_core/Protocol.h"
#include "im.pb.h"

#include <chrono>
#include <filesystem>
#include <iostream>

namespace imsrv {

using asio::ip::tcp;
using im::proto::DEF_PROT_FRIEND_OFFLINE;

Server::Server(std::uint16_t port, int ioThreadCount, int dbWorkers,
               std::string dbPath, std::string uploadDir)
    : m_port(port)
    , m_ioThreadCount(ioThreadCount > 0 ? ioThreadCount : 4)
    , m_dbWorkers(dbWorkers > 0 ? dbWorkers : 2)
    , m_dbPath(std::move(dbPath))
    , m_uploadDir(std::move(uploadDir))
    , m_acceptor(m_io)
    , m_signals(m_io)
{
}

Server::~Server()
{
    stop();
}

bool Server::start()
{
    // 数据目录与上传目录
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_dbPath).parent_path(), ec);
    std::filesystem::create_directories(m_uploadDir + "/img", ec);
    std::filesystem::create_directories(m_uploadDir + "/file", ec);
    std::filesystem::create_directories(m_uploadDir + "/file/tmp", ec);

    if (!m_db.open(m_dbPath, m_dbWorkers)) {
        log("[server] 打开数据库失败: ", m_dbPath);
        return false;
    }
    m_db.seedIfEmpty();

    m_dispatcher = std::make_unique<Dispatcher>(*this);

    // 业务线程池 + 业务 strand 池
    m_dbPool = std::make_unique<asio::thread_pool>(m_dbWorkers);
    for (int i = 0; i < m_dbWorkers; ++i) {
        m_bizStrands.push_back(std::make_shared<BizStrand>(asio::make_strand(m_dbPool->get_executor())));
    }

    // acceptor
    asio::error_code ec2;
    m_acceptor.open(tcp::v4(), ec2);
    if (ec2) { log("[server] open acceptor 失败: ", ec2.message()); return false; }
    m_acceptor.set_option(tcp::acceptor::reuse_address(true), ec2);
    m_acceptor.bind(tcp::endpoint(tcp::v4(), m_port), ec2);
    if (ec2) { log("[server] bind 失败: ", ec2.message()); return false; }
    m_acceptor.listen(100, ec2);
    if (ec2) { log("[server] listen 失败: ", ec2.message()); return false; }

    // 信号优雅关停：处理器在 IO 线程上执行，只置位标志；
    // 真正的 stop()（含 join 线程）由 run() 的调用方线程执行，避免 IO 线程 join 自身
    m_signals.add(SIGINT);
    m_signals.add(SIGTERM);
    m_signals.async_wait([this](const asio::error_code&, int) { m_stopRequested = true; });

    doAccept();
    m_running = true;

    // IO 线程池
    for (int i = 0; i < m_ioThreadCount; ++i) {
        m_ioThreads.emplace_back([this, i]() {
            log("[server] IO 线程 ", i, " 启动");
            m_io.run();
        });
    }

    // 心跳超时扫描线程（10s 周期，90s 超时）
    m_hbThread = std::thread(&Server::hbScanLoop, this);

    log("[server] 启动成功 port=", m_port,
        " ioThreads=", m_ioThreadCount,
        " dbWorkers=", m_dbWorkers,
        " db=", m_dbPath);
    return true;
}

void Server::run()
{
    // 等待停止信号（主线程阻塞，IO 在 m_ioThreads 上跑）；返回后调用方应调 stop()
    while (!m_stopRequested.load() && !m_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void Server::stop()
{
    if (m_stop.exchange(true)) return;
    log("[server] 正在关停...");

    // 先停 running 标志：阻止 accept 错误路径重新武装监听、心跳扫描线程退出
    m_running = false;

    asio::error_code ec;
    m_acceptor.close(ec);
    m_signals.cancel(ec);

    if (m_dbPool) {
        m_dbPool->stop();
        m_dbPool->join();
    }
    m_io.stop();
    for (auto& t : m_ioThreads) {
        if (t.joinable()) t.join();
    }
    if (m_hbThread.joinable()) m_hbThread.join();
    log("[server] 已关停");
}

std::shared_ptr<Server::BizStrand> Server::nextBizStrand()
{
    return m_bizStrands[m_nextBiz.fetch_add(1) % m_bizStrands.size()];
}

void Server::doAccept()
{
    m_acceptor.async_accept(
        [this](const asio::error_code& ec, tcp::socket socket) {
            if (ec) {
                if (m_running.load()) doAccept(); // 非关停错误继续监听
                return;
            }
            try {
                log("[server] 新连接: ", socket.remote_endpoint().address().to_string());
            } catch (...) {}
            auto session = std::make_shared<Session>(std::move(socket), *this, nextBizStrand());
            session->start();
            doAccept();
        });
}

void Server::dispatchPacket(const std::shared_ptr<Session>& s, std::uint32_t type, std::string payload)
{
    // 任何有效包都刷新活跃时间（心跳判定依据）
    if (s->userId() > 0) m_presence.touch(s->userId());
    m_dispatcher->handle(s, type, payload);
}

void Server::onSessionClosed(const std::shared_ptr<Session>& s)
{
    const int uid = s->userId();
    if (uid <= 0) return; // 未登录连接无需广播

    // 摘在线映射（仅当 map 中就是本 session，互踢场景不会误摘新连接）
    m_presence.offline(uid, s);

    // 广播好友下线（投递到业务 strand 执行 db 查询 + 转发，与正常业务同通道保序）
    asio::post(*m_bizStrands[uid % m_bizStrands.size()], [this, uid]() {
        im::proto::FriendOffline pkt;
        pkt.set_offlineid(uid);
        const std::string payload = pkt.SerializeAsString();
        for (const auto& f : m_db.getFriends(uid)) {
            if (auto fs = m_presence.get(f.id)) {
                fs->deliver(DEF_PROT_FRIEND_OFFLINE, payload);
            }
        }
    });
}

void Server::hbScanLoop()
{
    while (m_running.load()) {
        for (int i = 0; i < 100 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running.load()) break;

        auto stale = m_presence.scanStale(90);
        for (auto& kv : stale) {
            log("[server] 心跳超时，强制下线 id=", kv.first);
            kv.second->close(); // 关闭后 onSessionClosed 负责摘映射并广播
        }
    }
}

} // namespace imsrv
