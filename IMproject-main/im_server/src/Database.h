#pragma once
// SQLite 数据访问层：连接池 + WAL 并发配置 + 全部 SQL
// 对应考察点：服务端 db 字段/索引设计、SQLite 写并发（WAL + busy_timeout）
//
// 并发说明：SQLite 同时刻只允许一个写者。本类为连接池（每连接独立互斥锁），
// 每个连接打开时执行 journal_mode=WAL + busy_timeout=5000，
// 读写不互斥、写冲突等待而非立即报 SQLITE_BUSY。

#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

struct sqlite3;

namespace imsrv {

// 用户/好友信息（对应 pb FriendInfo 的业务形态）
struct UserRecord {
    int id = 0;
    int iconId = 0;
    std::string nick;
    std::string feeling;
};

struct FriendRecord {
    int id = 0;
    int iconId = 0;
    int status = 1; // 0 在线 1 离线（由 Presence 填充，db 不存）
    std::string nick;
    std::string feeling;
};

// 全量消息行（漫游/离线共用单表，is_delivered 区分）
struct StoredMessage {
    std::string msgId;
    int senderId = 0;
    int receiverId = 0;
    int type = 0;           // 0 文本 1 图片
    std::string content;    // 文本内容
    std::string mediaPath;  // 图片落盘路径
    int imgW = 0;
    int imgH = 0;
    std::int64_t ts = 0;
};

class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开数据库（连接池大小 poolSize），建表 + WAL 配置；重复打开安全
    bool open(const std::string& dbPath, int poolSize = 2);

    // 首次运行播种：3 个种子用户（密码 123456 加盐哈希）+ 好友关系，幂等
    void seedIfEmpty();

    // ---------------- 注册/登录 ----------------
    // 返回 proto::REGISTER_SUCC / REGISTER_NICK_EXIT / REGISTER_TEL_EXIT（-1 表示 db 错误）
    // passHash 为客户端已算的 sha256(明文)；服务端再 盐+二次哈希 存库
    int registerUser(const std::string& nick, const std::string& tel, const std::string& passHash);

    // 校验登录：outUserId 输出用户 id；返回 proto::LOGIN_SUCCESS / LOGIN_NOTEXIT / LOGIN_PASSERROR
    int loginUser(const std::string& tel, const std::string& passHash, int& outUserId);

    // ---------------- 资料/好友 ----------------
    bool getUser(int id, UserRecord& out);
    std::vector<FriendRecord> getFriends(int id);
    bool addFriendBidirectional(int idA, int idB);
    // 按昵称查用户 id（0=不存在）
    int getUserIdByNick(const std::string& nick);

    // ---------------- 消息（漫游/离线单表） ----------------
    // 保存消息；conversation_id 由收发双方 id 生成（min*K+max，双向同值）
    bool saveMessage(const StoredMessage& m, bool delivered);

    // 拉取未投递消息（按时间升序），并可标记为已投递
    std::vector<StoredMessage> pullUndelivered(int receiverId);
    void markDelivered(const std::vector<std::string>& msgIds);

private:
    struct Conn {
        sqlite3* db = nullptr;
        std::mutex mtx;
    };

    Conn& acquire(); // 轮询取连接
    bool execOn(sqlite3* db, const char* sql);

    std::vector<std::unique_ptr<Conn>> m_pool;
    std::atomic<size_t> m_next{0};
};

// conversation_id 生成：min(id)*K + max(id)，K 远大于用户数上限
inline std::int64_t makeConversationId(int idA, int idB)
{
    const std::int64_t lo = idA < idB ? idA : idB;
    const std::int64_t hi = idA < idB ? idB : idA;
    return lo * (1 << 20) + hi;
}

} // namespace imsrv
