#include "Database.h"
#include "sha256.h"

#include <sqlite3.h>
#include <ctime>
#include <random>

namespace imsrv {

// 协议结果码（与 client_core/Protocol.h、IMServer/def.h 保持一致，避免双端漂移）
namespace rc {
    constexpr int REGISTER_SUCC = 1;
    constexpr int REGISTER_NICK_EXIT = 2;
    constexpr int REGISTER_TEL_EXIT = 3;
    constexpr int LOGIN_SUCCESS = 0;
    constexpr int LOGIN_NOTEXIT = 1;
    constexpr int LOGIN_PASSERROR = 2;
}

namespace {

std::string generateSaltHex()
{
    std::random_device rd;
    unsigned char bytes[16];
    for (auto& b : bytes) b = static_cast<unsigned char>(rd() & 0xFF);
    static const char* hex = "0123456789abcdef";
    std::string salt;
    salt.reserve(32);
    for (unsigned char b : bytes) {
        salt.push_back(hex[(b >> 4) & 0x0F]);
        salt.push_back(hex[b & 0x0F]);
    }
    return salt;
}

} // namespace

Database::~Database()
{
    for (auto& c : m_pool) {
        if (c->db) sqlite3_close(c->db);
    }
}

bool Database::execOn(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

Database::Conn& Database::acquire()
{
    return *m_pool[m_next.fetch_add(1) % m_pool.size()];
}

bool Database::open(const std::string& dbPath, int poolSize)
{
    if (!m_pool.empty()) return true;
    if (poolSize < 1) poolSize = 1;

    for (int i = 0; i < poolSize; ++i) {
        auto conn = std::make_unique<Conn>();
        if (sqlite3_open(dbPath.c_str(), &conn->db) != SQLITE_OK) return false;

        // SQLite 写并发关键配置：WAL（读写不互斥）+ busy_timeout（写冲突等待而非报错）
        execOn(conn->db, "PRAGMA journal_mode=WAL;");
        execOn(conn->db, "PRAGMA busy_timeout=5000;");
        execOn(conn->db, "PRAGMA foreign_keys=ON;");

        m_pool.push_back(std::move(conn));
    }

    // 建表（在第一个连接上执行）
    Conn& c = *m_pool[0];
    std::lock_guard<std::mutex> lock(c.mtx);
    return execOn(c.db,
        "CREATE TABLE IF NOT EXISTS t_user("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  tel TEXT NOT NULL UNIQUE,"
        "  passwd TEXT NOT NULL,"
        "  salt TEXT NOT NULL DEFAULT '',"
        "  feeling TEXT NOT NULL DEFAULT '',"
        "  iconid INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS t_friend("
        "  idA INTEGER NOT NULL,"
        "  idB INTEGER NOT NULL,"
        "  PRIMARY KEY(idA, idB)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_friend_idB ON t_friend(idB);"
        // 全量消息表：漫游/离线单表，conversation_id 会话维度索引（对齐 QQNT PeerUidIndex+MsgTime）
        "CREATE TABLE IF NOT EXISTS messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  msg_id TEXT UNIQUE,"
        "  conversation_id INTEGER NOT NULL,"
        "  sender_id INTEGER NOT NULL,"
        "  receiver_id INTEGER NOT NULL,"
        "  type INTEGER NOT NULL DEFAULT 0,"
        "  content TEXT,"
        "  media_path TEXT,"
        "  img_w INTEGER NOT NULL DEFAULT 0,"
        "  img_h INTEGER NOT NULL DEFAULT 0,"
        "  ts INTEGER NOT NULL,"
        "  is_delivered INTEGER NOT NULL DEFAULT 0,"
        "  is_read INTEGER NOT NULL DEFAULT 0,"
        "  seq INTEGER NOT NULL DEFAULT 0,"
        "  file_id TEXT,"
        "  file_size INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_msg_conv_ts ON messages(conversation_id, ts);"
        "CREATE INDEX IF NOT EXISTS idx_msg_conv_seq ON messages(conversation_id, seq);"
        "CREATE INDEX IF NOT EXISTS idx_msg_recv ON messages(receiver_id, is_delivered, ts);"
        // 漫游会话列表（roamConversations）按 sender_id 分组取末条，需 sender_id 索引避免全表扫描
        "CREATE INDEX IF NOT EXISTS idx_msg_sender ON messages(sender_id);");
}

void Database::seedIfEmpty()
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    // 已有用户则跳过（幂等）
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(c.db, "SELECT COUNT(*) FROM t_user;", -1, &st, nullptr) != SQLITE_OK) return;
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (count > 0) return;

    struct Seed { const char* nick; const char* tel; const char* feeling; };
    const Seed users[] = {
        {"张三", "13800000001", "我是张三"},
        {"李四", "13800000002", "我是李四"},
        {"王五", "13800000003", "我是王五"},
    };

    execOn(c.db, "BEGIN;");
    int ids[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const std::string salt = generateSaltHex();
        // 客户端传 sha256(明文)，服务端存 sha256(salt + sha256(明文))；种子密码统一 123456
        const std::string stored = im::sha256Hex(salt + im::sha256Hex("123456"));
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(c.db,
            "INSERT INTO t_user(name, tel, passwd, salt, feeling, iconid) VALUES(?,?,?,?,?,?);",
            -1, &ins, nullptr);
        sqlite3_bind_text(ins, 1, users[i].nick, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, users[i].tel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, stored.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 4, salt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 5, users[i].feeling, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 6, i + 1);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
        ids[i] = static_cast<int>(sqlite3_last_insert_rowid(c.db));
    }
    // 好友关系（双向）：张三↔李四、李四↔王五
    const int pairs[][2] = {{ids[0], ids[1]}, {ids[1], ids[0]}, {ids[1], ids[2]}, {ids[2], ids[1]}};
    for (const auto& p : pairs) {
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(c.db, "INSERT OR IGNORE INTO t_friend(idA, idB) VALUES(?,?);", -1, &ins, nullptr);
        sqlite3_bind_int(ins, 1, p[0]);
        sqlite3_bind_int(ins, 2, p[1]);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }
    execOn(c.db, "COMMIT;");
}

int Database::registerUser(const std::string& nick, const std::string& tel, const std::string& passHash)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    // 昵称唯一性
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT 1 FROM t_user WHERE name=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (exists) return rc::REGISTER_NICK_EXIT;

    // 电话唯一性
    sqlite3_prepare_v2(c.db, "SELECT 1 FROM t_user WHERE tel=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, tel.c_str(), -1, SQLITE_TRANSIENT);
    exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (exists) return rc::REGISTER_TEL_EXIT;

    const std::string salt = generateSaltHex();
    const std::string stored = im::sha256Hex(salt + passHash);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(c.db,
        "INSERT INTO t_user(name, tel, passwd, salt, feeling, iconid) VALUES(?,?,?,?,?,3);",
        -1, &ins, nullptr);
    sqlite3_bind_text(ins, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, tel.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, stored.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 5, "努力实现财富自由", -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(ins) == SQLITE_DONE;
    sqlite3_finalize(ins);
    return ok ? rc::REGISTER_SUCC : -1;
}

int Database::loginUser(const std::string& tel, const std::string& passHash, int& outUserId)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT passwd, salt, id FROM t_user WHERE tel=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, tel.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return rc::LOGIN_NOTEXIT;
    }
    const std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const std::string salt = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    outUserId = sqlite3_column_int(st, 2);
    sqlite3_finalize(st);

    return im::sha256Hex(salt + passHash) == stored ? rc::LOGIN_SUCCESS : rc::LOGIN_PASSERROR;
}

bool Database::getUser(int id, UserRecord& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT id, iconid, name, feeling FROM t_user WHERE id=?;", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.id = sqlite3_column_int(st, 0);
    out.iconId = sqlite3_column_int(st, 1);
    const unsigned char* nick = sqlite3_column_text(st, 2);
    const unsigned char* feeling = sqlite3_column_text(st, 3);
    out.nick = nick ? reinterpret_cast<const char*>(nick) : "";
    out.feeling = feeling ? reinterpret_cast<const char*>(feeling) : "";
    sqlite3_finalize(st);
    return true;
}

std::vector<FriendRecord> Database::getFriends(int id)
{
    std::vector<FriendRecord> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT u.id, u.iconid, u.name, u.feeling FROM t_friend f "
        "JOIN t_user u ON u.id = f.idB WHERE f.idA = ?;",
        -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        FriendRecord fr;
        fr.id = sqlite3_column_int(st, 0);
        fr.iconId = sqlite3_column_int(st, 1);
        const unsigned char* nick = sqlite3_column_text(st, 2);
        const unsigned char* feeling = sqlite3_column_text(st, 3);
        fr.nick = nick ? reinterpret_cast<const char*>(nick) : "";
        fr.feeling = feeling ? reinterpret_cast<const char*>(feeling) : "";
        out.push_back(std::move(fr));
    }
    sqlite3_finalize(st);
    return out;
}

bool Database::addFriendBidirectional(int idA, int idB)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    execOn(c.db, "BEGIN;");
    for (const auto& p : {std::pair<int,int>{idA, idB}, {idB, idA}}) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(c.db, "INSERT OR IGNORE INTO t_friend(idA, idB) VALUES(?,?);", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, p.first);
        sqlite3_bind_int(st, 2, p.second);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return execOn(c.db, "COMMIT;");
}

int Database::getUserIdByNick(const std::string& nick)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "SELECT id FROM t_user WHERE name=?;", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
    int id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

bool Database::saveMessage(StoredMessage& m, bool delivered)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    const std::int64_t convId = makeConversationId(m.senderId, m.receiverId);

    // 幂等：若该 msg_id 已存在（漫游/重发），直接读回已有 seq，不再分配新号
    {
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(c.db, "SELECT seq FROM messages WHERE msg_id=?;", -1, &q, nullptr);
        sqlite3_bind_text(q, 1, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(q) == SQLITE_ROW) {
            m.seq = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
            return true;
        }
        sqlite3_finalize(q);
    }

    // 会话级单调递增 seq：当前会话 MAX(seq)+1（同连接锁内串行，保证不重号）
    std::int64_t nextSeq = 1;
    {
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(c.db,
            "SELECT COALESCE(MAX(seq),0)+1 FROM messages WHERE conversation_id=?;", -1, &q, nullptr);
        sqlite3_bind_int64(q, 1, convId);
        if (sqlite3_step(q) == SQLITE_ROW) nextSeq = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    m.seq = nextSeq;

    sqlite3_stmt* st = nullptr;
    // msg_id UNIQUE + INSERT OR IGNORE：漫游/重发幂等
    sqlite3_prepare_v2(c.db,
        "INSERT OR IGNORE INTO messages"
        "(msg_id, conversation_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, is_delivered, is_read, seq, file_id, file_size)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,0,?,?,?);",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, m.msgId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, convId);
    sqlite3_bind_int(st, 3, m.senderId);
    sqlite3_bind_int(st, 4, m.receiverId);
    sqlite3_bind_int(st, 5, m.type);
    sqlite3_bind_text(st, 6, m.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, m.mediaPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 8, m.imgW);
    sqlite3_bind_int(st, 9, m.imgH);
    sqlite3_bind_int64(st, 10, m.ts);
    sqlite3_bind_int(st, 11, delivered ? 1 : 0);
    sqlite3_bind_int64(st, 12, m.seq);
    sqlite3_bind_text(st, 13, m.fileId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 14, m.fileSize);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

std::vector<StoredMessage> Database::pullUndelivered(int receiverId)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq "
        "FROM messages WHERE receiver_id=? AND is_delivered=0 ORDER BY seq ASC;",
        -1, &st, nullptr);
    sqlite3_bind_int(st, 1, receiverId);
    while (sqlite3_step(st) == SQLITE_ROW) {
        StoredMessage m;
        const unsigned char* msgId = sqlite3_column_text(st, 0);
        m.msgId = msgId ? reinterpret_cast<const char*>(msgId) : "";
        m.senderId = sqlite3_column_int(st, 1);
        m.receiverId = sqlite3_column_int(st, 2);
        m.type = sqlite3_column_int(st, 3);
        const unsigned char* content = sqlite3_column_text(st, 4);
        const unsigned char* path = sqlite3_column_text(st, 5);
        m.content = content ? reinterpret_cast<const char*>(content) : "";
        m.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
        m.imgW = sqlite3_column_int(st, 6);
        m.imgH = sqlite3_column_int(st, 7);
        m.ts = sqlite3_column_int64(st, 8);
        m.seq = sqlite3_column_int64(st, 9);
        out.push_back(std::move(m));
    }
    sqlite3_finalize(st);
    return out;
}

void Database::markDelivered(const std::vector<std::string>& msgIds)
{
    if (msgIds.empty()) return;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db, "UPDATE messages SET is_delivered=1 WHERE msg_id=?;", -1, &st, nullptr);
    for (const auto& id : msgIds) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

// 从结果集当前行装载一条 StoredMessage（列序须与 SELECT 一致：
// msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq）
static StoredMessage readMessageRow(sqlite3_stmt* st)
{
    StoredMessage m;
    const unsigned char* msgId = sqlite3_column_text(st, 0);
    m.msgId = msgId ? reinterpret_cast<const char*>(msgId) : "";
    m.senderId = sqlite3_column_int(st, 1);
    m.receiverId = sqlite3_column_int(st, 2);
    m.type = sqlite3_column_int(st, 3);
    const unsigned char* content = sqlite3_column_text(st, 4);
    const unsigned char* path = sqlite3_column_text(st, 5);
    m.content = content ? reinterpret_cast<const char*>(content) : "";
    m.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
    m.imgW = sqlite3_column_int(st, 6);
    m.imgH = sqlite3_column_int(st, 7);
    m.ts = sqlite3_column_int64(st, 8);
    m.seq = sqlite3_column_int64(st, 9);
    return m;
}

std::vector<StoredMessage> Database::roamConversations(int userId)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    // 每会话末条：内层两个子查询各走 idx_msg_sender / idx_msg_recv 避免 OR 全表扫描，
    // UNION ALL 合并后按 conversation_id 取全局 MAX(id)（同会话我发/我收各一末条时取更晚者）。
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT m.msg_id, m.sender_id, m.receiver_id, m.type, m.content, m.media_path, "
        "m.img_w, m.img_h, m.ts, m.seq FROM messages m JOIN ("
        "  SELECT conversation_id, MAX(id) AS mid FROM ("
        "    SELECT conversation_id, id FROM messages WHERE sender_id=?"
        "    UNION ALL"
        "    SELECT conversation_id, id FROM messages WHERE receiver_id=?"
        "  ) GROUP BY conversation_id"
        ") t ON m.id = t.mid ORDER BY m.ts DESC;",
        -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId);
    sqlite3_bind_int(st, 2, userId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readMessageRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<StoredMessage> Database::roamMessages(int userId, int peerId, std::int64_t beforeSeq, int limit)
{
    std::vector<StoredMessage> out;
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);

    const std::int64_t convId = makeConversationId(userId, peerId);

    // 会话内比游标更早的 N 条（seq 倒序），走 idx_msg_conv_seq
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq "
        "FROM messages WHERE conversation_id=? AND seq < ? ORDER BY seq DESC LIMIT ?;",
        -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, convId);
    sqlite3_bind_int64(st, 2, beforeSeq);
    sqlite3_bind_int(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(readMessageRow(st));
    sqlite3_finalize(st);
    return out;
}

bool Database::getMessageByMsgId(const std::string& msgId, StoredMessage& out)
{
    Conn& c = acquire();
    std::lock_guard<std::mutex> lock(c.mtx);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(c.db,
        "SELECT msg_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, seq, file_id, file_size "
        "FROM messages WHERE msg_id=?;",
        -1, &st, nullptr);
    sqlite3_bind_text(st, 1, msgId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* mid = sqlite3_column_text(st, 0);
        out.msgId = mid ? reinterpret_cast<const char*>(mid) : "";
        out.senderId = sqlite3_column_int(st, 1);
        out.receiverId = sqlite3_column_int(st, 2);
        out.type = sqlite3_column_int(st, 3);
        const unsigned char* content = sqlite3_column_text(st, 4);
        const unsigned char* path = sqlite3_column_text(st, 5);
        out.content = content ? reinterpret_cast<const char*>(content) : "";
        out.mediaPath = path ? reinterpret_cast<const char*>(path) : "";
        out.imgW = sqlite3_column_int(st, 6);
        out.imgH = sqlite3_column_int(st, 7);
        out.ts = sqlite3_column_int64(st, 8);
        out.seq = sqlite3_column_int64(st, 9);
        const unsigned char* fid = sqlite3_column_text(st, 10);
        out.fileId = fid ? reinterpret_cast<const char*>(fid) : "";
        out.fileSize = sqlite3_column_int64(st, 11);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

} // namespace imsrv
