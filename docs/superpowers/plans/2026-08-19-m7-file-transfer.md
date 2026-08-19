# M7 文件传输 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在即通中实现任意类型文件的分片收发，支持顺序水位线断点续传、进度显示、消息卡片+按需下载，复用现有单条 TCP。

**Architecture:** 独立分片协议（协议号 1017~1022），文件卡片复用 `ChatInfoRq(type=FILE)` 走现有转发/离线/漫游链路。发送方顺序发 ≤256KB 块，服务端 `.part` 顺序追加、以文件大小推算水位线 N；`FileCompleteRq` 时校验整文件 sha256+大小后落盘并建消息。接收方收到卡片、点击后从本地 `.part` 块数续拉。服务端逻辑由 C++ `test_e2e` 回环驱动（TDD），Android 端编译 + 手测。

**Tech Stack:** C++17 + asio + SQLite（服务端/client_core）、protobuf3、Kotlin + Jetpack Compose + Room + protobuf-javalite（Android）。

## Global Constraints

- 线格式：`[4B 大端包长][4B 小端协议号][pb payload]`，`MAX_PACK_LEN = 10MB`。
- 协议号 `DEF_BASE=1000`，`DEF_PROT_COUNT=30`；M7 占用 **1017~1022**（1013~1016 已被 M6 漫游占用，勿复用）。
- 分块大小固定 `CHUNK_SIZE = 256*1024`（256KB）。文件大小上限 `MAX_FILE_SIZE = 100*1024*1024`（100MB）。
- `file_id == msg_id`（客户端生成的 UUID）。
- 协议号常量必须三处同步：`client_core/include/client_core/Protocol.h`、`jitong_android/app/src/main/java/com/jitong/im/net/Protocol.kt`；`im.proto` 改后用 `protoc --cpp_out=protocol/generated --proto_path=protocol protocol/im.proto` 重新生成（protoc 主版本须与运行时一致，本机 35.1）。
- **git 仓库根在父目录** `/Users/xiaozong/zzq/IM Project/`（工作子目录为 `IMproject-main/`），`git add` 路径带前缀。
- 服务端 e2e 构建：`cd im_server && cmake --build build --target test_e2e`（增量构建偶发 stale .o，链接报未定义符号时 `touch` 对应 .cpp 再构建）。
- Android 构建：`JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"`，gradle 用 dist 缓存 `~/.gradle/wrapper/dists/gradle-8.9-bin/*/gradle-8.9/bin/gradle`（无 `gradlew`）。
- sha256：C++ 复用 `protocol/sha256.h` 的增量类 `im::Sha256`（`update()`/`final()`）；Kotlin 复用 `net/Sha256.kt`，为大文件新增流式变体。

---

### Task 1: 协议定义 + 协议号常量 + 重新生成

**Files:**
- Modify: `protocol/im.proto`（新增 6 message + 扩 `ChatInfoRq`）
- Modify: `protocol/generated/im.pb.cc`, `protocol/generated/im.pb.h`（protoc 重新生成）
- Modify: `client_core/include/client_core/Protocol.h:44`（协议号常量后追加）
- Modify: `jitong_android/app/src/main/java/com/jitong/im/net/Protocol.kt:24`（常量后追加）

**Interfaces:**
- Produces（pb 消息，供全部后续任务）：`im::proto::FileOfferRq/FileOfferRs/FileChunkRq/FileCompleteRq/FileProgressRs/FileDownloadRq`；`ChatInfoRq` 新增 `file_name`(11)/`file_size`(12)/`file_id`(13)。
- Produces（协议号）：`DEF_PROT_FILE_OFFER_RQ=1017 … DEF_PROT_FILE_DOWNLOAD_RQ=1022`（C++）；`Protocol.FILE_OFFER_RQ=1017 …`（Kotlin）。
- Produces（结果码）：`FILE_OFFER_OK=0`、`FILE_OFFER_TOO_LARGE=1`；进度 status：`FILE_ST_UPLOADING=0`/`FILE_ST_VERIFYING=1`/`FILE_ST_DONE=2`/`FILE_ST_FAILED=3`。

- [ ] **Step 1: 在 `protocol/im.proto` 的 `FriendOffline` 之后、漫游 message 之前插入文件消息定义**

```proto
// ---------------- 文件传输（M7） ----------------
// 独立分片协议：文件可 ≥10MB，内联会堵死信令通道。分块 ≤256KB，顺序水位线断点续传，
// 消息体卡片复用 ChatInfoRq(type=FILE)。file_id == msg_id（客户端 UUID）。

message FileOfferRq {   // 1017 C→S：发起/续传协商
  string msg_id = 1;    // 客户端 UUID，全程幂等 / 重组 key / = file_id
  int32  receiver_id = 2;
  string file_name = 3; // UTF-8
  int64  file_size = 4; // 字节
  int32  total_chunks = 5;
  string sha256 = 6;    // 整文件完整性校验（小写 hex）
}

message FileOfferRs {   // 1018 S→C
  string msg_id = 1;
  string file_id = 2;          // = msg_id
  int32  received_chunks = 3;  // 水位线 N（续传起点）
  int32  result = 4;           // proto::FILE_OFFER_OK / FILE_OFFER_TOO_LARGE
}

message FileChunkRq {   // 1019 C→S（上传）/ S→C（下载）复用
  string file_id = 1;
  int32  chunk_index = 2;
  bytes  data = 3;             // ≤256KB
}

message FileCompleteRq { // 1020 C→S：请求校验+落盘+建消息
  string file_id = 1;
  string msg_id = 2;
}

message FileProgressRs { // 1021 S→C：上传/下载进度共用
  string file_id = 1;
  int32  received_chunks = 2;
  int32  total_chunks = 3;
  int32  status = 4;          // proto::FILE_ST_*
}

message FileDownloadRq { // 1022 C→S：接收方发起/续传下载
  string file_id = 1;
  int32  from_chunk = 2;      // 续传起点 = 本地已存块数
}
```

- [ ] **Step 2: 扩展 `ChatInfoRq`（文件卡片字段）**

在 `protocol/im.proto` 的 `message ChatInfoRq { ... }` 末尾（`seq = 10` 之后）追加：

```proto
  string file_name = 11;   // type=FILE 时：文件名
  int64  file_size = 12;   // type=FILE 时：字节数
  string file_id = 13;     // type=FILE 时：服务端文件标识（=发送方 msg_id）
```

- [ ] **Step 3: 重新生成 C++ pb 代码**

Run:
```bash
cd "/Users/xiaozong/zzq/IM Project/IMproject-main"
protoc --cpp_out=protocol/generated --proto_path=protocol protocol/im.proto
```
Expected: 无输出。验证：`grep -c "FileOfferRq" protocol/generated/im.pb.h` 输出 > 0。

- [ ] **Step 4: 追加 C++ 协议号常量**

在 `client_core/include/client_core/Protocol.h` 的 `DEF_PROT_ROAM_MSG_RS = DEF_BASE + 16;` 之后追加：

```cpp
constexpr protType DEF_PROT_FILE_OFFER_RQ    = DEF_BASE + 17; // 文件协商请求
constexpr protType DEF_PROT_FILE_OFFER_RS    = DEF_BASE + 18; // 文件协商响应（含水位线）
constexpr protType DEF_PROT_FILE_CHUNK_RQ    = DEF_BASE + 19; // 文件分片（上/下行复用）
constexpr protType DEF_PROT_FILE_COMPLETE_RQ = DEF_BASE + 20; // 上传完成请求
constexpr protType DEF_PROT_FILE_PROGRESS_RS = DEF_BASE + 21; // 传输进度
constexpr protType DEF_PROT_FILE_DOWNLOAD_RQ = DEF_BASE + 22; // 下载请求
```

在结果码区（`ADD_FRIEND_NOTEXIT = 3;` 之后）追加：

```cpp
// 文件传输
constexpr int FILE_OFFER_OK        = 0;
constexpr int FILE_OFFER_TOO_LARGE = 1;
constexpr int FILE_ST_UPLOADING = 0;
constexpr int FILE_ST_VERIFYING = 1;
constexpr int FILE_ST_DONE      = 2;
constexpr int FILE_ST_FAILED    = 3;
constexpr std::size_t FILE_CHUNK_SIZE = 256 * 1024;      // 256KB
constexpr std::int64_t FILE_MAX_SIZE  = 100LL * 1024 * 1024; // 100MB
```

- [ ] **Step 5: 追加 Kotlin 协议号常量**

在 `jitong_android/app/src/main/java/com/jitong/im/net/Protocol.kt` 的 `ROAM_MSG_RS = DEF_BASE + 16` 之后追加：

```kotlin
    const val FILE_OFFER_RQ = DEF_BASE + 17
    const val FILE_OFFER_RS = DEF_BASE + 18
    const val FILE_CHUNK_RQ = DEF_BASE + 19
    const val FILE_COMPLETE_RQ = DEF_BASE + 20
    const val FILE_PROGRESS_RS = DEF_BASE + 21
    const val FILE_DOWNLOAD_RQ = DEF_BASE + 22

    const val FILE_OFFER_OK = 0
    const val FILE_OFFER_TOO_LARGE = 1
    const val FILE_ST_UPLOADING = 0
    const val FILE_ST_VERIFYING = 1
    const val FILE_ST_DONE = 2
    const val FILE_ST_FAILED = 3
    const val FILE_CHUNK_SIZE = 256 * 1024
    const val FILE_MAX_SIZE = 100L * 1024 * 1024
```

- [ ] **Step 6: 验证 C++ 编译（协议库）**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && cmake --build build --target im_protocol`
Expected: `Built target im_protocol`。

- [ ] **Step 7: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/protocol/im.proto IMproject-main/protocol/generated/im.pb.cc IMproject-main/protocol/generated/im.pb.h IMproject-main/client_core/include/client_core/Protocol.h IMproject-main/jitong_android/app/src/main/java/com/jitong/im/net/Protocol.kt
git commit -m "M7-1: 文件传输协议定义 + 协议号常量(1017~1022) + 重新生成 pb"
```

---

### Task 2: Database —— 文件列 + 文件消息落库

**Files:**
- Modify: `im_server/src/Database.h`（`StoredMessage` 加字段 + 新方法声明）
- Modify: `im_server/src/Database.cpp`（建表加列、`saveMessage` 绑定新列、新增 `getMessageByMsgId`）

**Interfaces:**
- Consumes: Task 1 的常量。
- Produces:
  - `StoredMessage` 新增 `std::string fileId; std::int64_t fileSize = 0;`
  - `bool Database::getMessageByMsgId(const std::string& msgId, StoredMessage& out);`（下载时按 file_id 取成品消息元数据）
  - `saveMessage` 现有签名不变，内部持久化 `file_id/file_size`。

- [ ] **Step 1: 在 `im_server/src/Database.h` 的 `StoredMessage` 结构追加文件字段**

在 `std::int64_t seq = 0;` 之后（`struct StoredMessage` 内）追加：

```cpp
    std::string fileId;          // type=2 文件消息：服务端文件标识（=发送方 msg_id）
    std::int64_t fileSize = 0;   // type=2：文件字节数
```

- [ ] **Step 2: 在 `im_server/src/Database.h` 声明按 msg_id 查询**

在 `std::vector<StoredMessage> roamMessages(...);` 之后追加：

```cpp
    // 按 msg_id 取单条消息（下载寻址：file_id==msg_id）；不存在返回 false
    bool getMessageByMsgId(const std::string& msgId, StoredMessage& out);
```

- [ ] **Step 3: 建表语句加两列**

在 `im_server/src/Database.cpp` 的 `CREATE TABLE IF NOT EXISTS messages(` 定义里，把 `"  seq INTEGER NOT NULL DEFAULT 0"` 改为：

```cpp
        "  seq INTEGER NOT NULL DEFAULT 0,"
        "  file_id TEXT,"
        "  file_size INTEGER NOT NULL DEFAULT 0"
```

> 注：新库直接建全列。已有旧库无需迁移（服务端 e2e 每次用新 `/tmp` 库）。

- [ ] **Step 4: `saveMessage` 写入新列**

在 `im_server/src/Database.cpp` 的 `saveMessage` 里，把 INSERT 语句与绑定扩展到 file 列。将 INSERT 的列清单/占位符和绑定改为（在 `seq` 之后加 `file_id, file_size`）：

```cpp
    sqlite3_prepare_v2(c.db,
        "INSERT OR IGNORE INTO messages"
        "(msg_id, conversation_id, sender_id, receiver_id, type, content, media_path, img_w, img_h, ts, is_delivered, is_read, seq, file_id, file_size)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,0,?,?,?);",
        -1, &st, nullptr);
    // ... 现有 bind 1..12 保持 ...
    sqlite3_bind_int64(st, 12, m.seq);
    sqlite3_bind_text(st, 13, m.fileId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 14, m.fileSize);
```

> 现有绑定里 `bind` 序号 11 是 `is_delivered`、12 是 `seq`（`is_read` 硬编码 0 不占位）。核对 `.cpp` 现状后保持前 12 个不变，仅追加 13、14。

- [ ] **Step 5: 实现 `getMessageByMsgId`（在 `roamMessages` 之后）**

```cpp
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
```

- [ ] **Step 6: 编译服务端库**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && cmake --build build --target im_server_lib`
Expected: `Built target im_server_lib`。

- [ ] **Step 7: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/src/Database.h IMproject-main/im_server/src/Database.cpp
git commit -m "M7-2: messages 表加 file_id/file_size 列 + getMessageByMsgId"
```

---

### Task 3: C++ ClientCore/IClientEvents —— 文件收发 API（供 e2e 驱动）

**Files:**
- Modify: `client_core/include/client_core/Types.h`（文件事件数据结构）
- Modify: `client_core/include/client_core/ClientCore.h`（回调 + 发送方法 + handler 声明）
- Modify: `client_core/src/ClientCore.cpp`（initFunArr 注册 + 发送实现 + handler 实现）

**Interfaces:**
- Consumes: Task 1 pb + 协议号。
- Produces（供 Task 4~7 e2e 使用）：
  - `IClientEvents` 新增（默认空实现）：`onFileOfferResult(const FileOfferInfo&)`、`onFileChunk(const std::string& fileId, int chunkIndex, const std::string& data)`、`onFileProgress(const std::string& fileId, int received, int total, int status)`。
  - `ClientCore` 新增：`void sendFileOffer(const std::string& msgId, int receiverId, const std::string& name, std::int64_t size, int totalChunks, const std::string& sha256);`、`void sendFileChunk(const std::string& fileId, int index, const std::string& data);`、`void sendFileComplete(const std::string& fileId, const std::string& msgId);`、`void sendFileDownload(const std::string& fileId, int fromChunk);`
  - `struct FileOfferInfo { std::string msgId, fileId; int receivedChunks; int result; };`（在 Types.h）

- [ ] **Step 1: `Types.h` 加 `FileOfferInfo`（在 `RoamMessage` 之后、`} // namespace im` 之前）**

```cpp
// 文件协商结果（含断点续传水位线）
struct FileOfferInfo {
    std::string msgId;
    std::string fileId;
    int receivedChunks = 0; // 水位线 N
    int result = 0;         // proto::FILE_OFFER_OK / FILE_OFFER_TOO_LARGE
};
```

- [ ] **Step 2: `ClientCore.h` 加回调（在 `onRoamMessages` 之后、`};` 之前）**

```cpp
    // 文件协商结果（含断点续传起点）
    virtual void onFileOfferResult(const FileOfferInfo& info) { (void)info; }
    // 收到文件分片（下载时 S→C；data 为该块字节）
    virtual void onFileChunk(const std::string& fileId, int chunkIndex, const std::string& data)
    { (void)fileId; (void)chunkIndex; (void)data; }
    // 传输进度（上传/下载共用）
    virtual void onFileProgress(const std::string& fileId, int received, int total, int status)
    { (void)fileId; (void)received; (void)total; (void)status; }
```

- [ ] **Step 3: `ClientCore.h` 加发送方法声明（在 `sendRoamMsgRq(...)` 之后）**

```cpp
    // ---------------- 文件传输（M7） ----------------
    void sendFileOffer(const std::string& msgId, int receiverId, const std::string& name,
                       std::int64_t size, int totalChunks, const std::string& sha256);
    void sendFileChunk(const std::string& fileId, int index, const std::string& data);
    void sendFileComplete(const std::string& fileId, const std::string& msgId);
    void sendFileDownload(const std::string& fileId, int fromChunk);
```

- [ ] **Step 4: `ClientCore.h` 加 handler 声明（在 `onRoamMsgRs(...)` 之后）**

```cpp
    void onFileOfferRs(const char* data, std::size_t len);
    void onFileChunkRq(const char* data, std::size_t len);
    void onFileProgressRs(const char* data, std::size_t len);
```

- [ ] **Step 5: `ClientCore.cpp` initFunArr 注册（在 `onRoamMsgRs` 注册之后）**

```cpp
    m_dealFunArr[DEF_PROT_FILE_OFFER_RS    - DEF_BASE] = &ClientCore::onFileOfferRs;
    m_dealFunArr[DEF_PROT_FILE_CHUNK_RQ    - DEF_BASE] = &ClientCore::onFileChunkRq;
    m_dealFunArr[DEF_PROT_FILE_PROGRESS_RS - DEF_BASE] = &ClientCore::onFileProgressRs;
```

- [ ] **Step 6: `ClientCore.cpp` 发送方法实现（在 `sendRoamMsgRq` 之后）**

```cpp
void ClientCore::sendFileOffer(const std::string& msgId, int receiverId, const std::string& name,
                               std::int64_t size, int totalChunks, const std::string& sha256)
{
    im::proto::FileOfferRq rq;
    rq.set_msg_id(msgId);
    rq.set_receiver_id(receiverId);
    rq.set_file_name(name);
    rq.set_file_size(size);
    rq.set_total_chunks(totalChunks);
    rq.set_sha256(sha256);
    sendPacket(DEF_PROT_FILE_OFFER_RQ, rq.SerializeAsString());
}

void ClientCore::sendFileChunk(const std::string& fileId, int index, const std::string& data)
{
    im::proto::FileChunkRq rq;
    rq.set_file_id(fileId);
    rq.set_chunk_index(index);
    rq.set_data(data);
    sendPacket(DEF_PROT_FILE_CHUNK_RQ, rq.SerializeAsString());
}

void ClientCore::sendFileComplete(const std::string& fileId, const std::string& msgId)
{
    im::proto::FileCompleteRq rq;
    rq.set_file_id(fileId);
    rq.set_msg_id(msgId);
    sendPacket(DEF_PROT_FILE_COMPLETE_RQ, rq.SerializeAsString());
}

void ClientCore::sendFileDownload(const std::string& fileId, int fromChunk)
{
    im::proto::FileDownloadRq rq;
    rq.set_file_id(fileId);
    rq.set_from_chunk(fromChunk);
    sendPacket(DEF_PROT_FILE_DOWNLOAD_RQ, rq.SerializeAsString());
}
```

- [ ] **Step 7: `ClientCore.cpp` handler 实现（在 `onRoamMsgRs` 之后、`} // namespace im` 之前）**

```cpp
void ClientCore::onFileOfferRs(const char* data, std::size_t len)
{
    im::proto::FileOfferRs rs;
    if (!parsePayload(data, len, rs)) return;
    FileOfferInfo info;
    info.msgId = rs.msg_id();
    info.fileId = rs.file_id();
    info.receivedChunks = rs.received_chunks();
    info.result = rs.result();
    if (auto* ev = m_events.load()) ev->onFileOfferResult(info);
}

void ClientCore::onFileChunkRq(const char* data, std::size_t len)
{
    im::proto::FileChunkRq rq;
    if (!parsePayload(data, len, rq)) return;
    if (auto* ev = m_events.load()) ev->onFileChunk(rq.file_id(), rq.chunk_index(), rq.data());
}

void ClientCore::onFileProgressRs(const char* data, std::size_t len)
{
    im::proto::FileProgressRs rs;
    if (!parsePayload(data, len, rs)) return;
    if (auto* ev = m_events.load())
        ev->onFileProgress(rs.file_id(), rs.received_chunks(), rs.total_chunks(), rs.status());
}
```

- [ ] **Step 8: 编译 client_core**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && cmake --build build --target client_core`
Expected: `Built target client_core`。

- [ ] **Step 9: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/client_core/include/client_core/Types.h IMproject-main/client_core/include/client_core/ClientCore.h IMproject-main/client_core/src/ClientCore.cpp
git commit -m "M7-3: ClientCore/IClientEvents 文件收发 API（供 e2e 驱动）"
```

---

### Task 4: Dispatcher —— 上传（Offer/Chunk/Complete）+ e2e 上传闭环

TDD：先写 e2e 上传断言（失败），再实现服务端 handler 至通过。

**Files:**
- Modify: `im_server/tests/test_e2e.cpp`（`RecordingEvents` 加文件收集器 + M7 上传段）
- Modify: `im_server/src/Dispatcher.h`（handler 声明）
- Modify: `im_server/src/Dispatcher.cpp`（注册 + `onFileOfferRq/onFileChunkRq/onFileCompleteRq` + 复用 `fillChatInfo` 转发卡片）
- Modify: `im_server/src/Server.h` / `Server.cpp`（`uploadDir()/img,file,file/tmp` 目录已建，补 `file/tmp`）

**Interfaces:**
- Consumes: Task 2 `Database::saveMessage/getMessageByMsgId`、Task 3 ClientCore 发送方法与回调。
- Produces: 服务端能接收顺序分片、水位线累积、Complete 校验落盘、转发/存储文件卡片。

- [ ] **Step 1: `test_e2e.cpp` 的 `RecordingEvents` 加文件收集器**

在 `struct ImageMsg {...}; std::vector<ImageMsg> images;` 之后追加：

```cpp
    // 文件收集
    im::FileOfferInfo lastOffer; bool gotOffer = false;
    std::vector<std::pair<int,std::string>> downloadChunks; // (chunkIndex, data)
    int lastProgressStatus = -1; int lastProgressRecv = -1;
```

在回调区（`onRoamMessages` override 之后）追加：

```cpp
    void onFileOfferResult(const im::FileOfferInfo& i) override {
        std::lock_guard<std::mutex> l(mtx); lastOffer = i; gotOffer = true; notify();
    }
    void onFileChunk(const std::string&, int idx, const std::string& d) override {
        std::lock_guard<std::mutex> l(mtx); downloadChunks.emplace_back(idx, d); notify();
    }
    void onFileProgress(const std::string&, int recv, int, int status) override {
        std::lock_guard<std::mutex> l(mtx); lastProgressRecv = recv; lastProgressStatus = status; notify();
    }
```

- [ ] **Step 2: 在 `test_e2e.cpp` 的送达回执断言之后、`a2.disconnect();` 之前插入 M7 上传段（失败测试）**

```cpp
    // ============ M7：文件上传（分片 + 水位线 + Complete 校验 + 卡片转发） ============
    // 造一个 300KB 文件（2 块：256KB + 44KB），a2(张三 idA) 发给 idB
    std::string fileBytes(300 * 1024, '\0');
    for (size_t i = 0; i < fileBytes.size(); ++i) fileBytes[i] = static_cast<char>((i * 31 + 7) % 256);
    const int chunkSz = 256 * 1024;
    const int totalChunks = (static_cast<int>(fileBytes.size()) + chunkSz - 1) / chunkSz; // 2
    const std::string fileSha = im::sha256Hex(fileBytes);
    const std::string fmsgId = "file-e2e-0001";

    a2.sendFileOffer(fmsgId, idB, "report.bin", (std::int64_t)fileBytes.size(), totalChunks, fileSha);
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId; }));
    assert(ea2.lastOffer.result == FILE_OFFER_OK);
    assert(ea2.lastOffer.receivedChunks == 0); // 新文件水位线 0

    for (int i = 0; i < totalChunks; ++i) {
        size_t off = (size_t)i * chunkSz;
        size_t len = std::min((size_t)chunkSz, fileBytes.size() - off);
        a2.sendFileChunk(fmsgId, i, fileBytes.substr(off, len));
    }
    a2.sendFileComplete(fmsgId, fmsgId);
    // 发送方应收到 done 进度
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_DONE; }));
    // 接收方 b3(李四) 应收到文件卡片（ChatInfoRq type=FILE，走在线转发）
    // 注：此处 b3 已在前面登录在线
```

> 若接收方在 M3 段已断开，请改用当前在线的接收方 session；实现时以 e2e 现存的在线双方为准（见 Step 6 校正）。

- [ ] **Step 3: 运行 e2e 验证失败**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && cmake --build build --target test_e2e && ./build/test_e2e`
Expected: 断言失败（服务端未注册 1017 handler，`gotOffer` 永不为真，超时 assert 失败）。

- [ ] **Step 4: `Dispatcher.h` 声明文件 handler（在 `onRoamMsgRq` 之后）**

```cpp
    void onFileOfferRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileChunkRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileCompleteRq(const std::shared_ptr<Session>& s, const std::string& payload);
    void onFileDownloadRq(const std::shared_ptr<Session>& s, const std::string& payload);
    // 文件路径工具
    std::string filePartPath(const std::string& fileId) const;   // uploads/file/tmp/<id>.part
    std::string fileFinalPath(const std::string& fileId, const std::string& name) const; // uploads/file/<id>_<name>
```

- [ ] **Step 5: `Dispatcher.cpp` 注册（在 `onRoamMsgRq` 注册之后）**

```cpp
    m_handlers[DEF_PROT_FILE_OFFER_RQ]    = [this](auto& s, auto& p) { onFileOfferRq(s, p); };
    m_handlers[DEF_PROT_FILE_CHUNK_RQ]    = [this](auto& s, auto& p) { onFileChunkRq(s, p); };
    m_handlers[DEF_PROT_FILE_COMPLETE_RQ] = [this](auto& s, auto& p) { onFileCompleteRq(s, p); };
    m_handlers[DEF_PROT_FILE_DOWNLOAD_RQ] = [this](auto& s, auto& p) { onFileDownloadRq(s, p); };
```

- [ ] **Step 6: `Dispatcher.cpp` 实现上传三 handler（在漫游 handler 之后，`} // namespace imsrv` 之前）**

需要头部加 `#include <sys/stat.h>` 不必，用 `std::filesystem`（已 include）。实现：

```cpp
std::string Dispatcher::filePartPath(const std::string& fileId) const
{
    return m_server.uploadDir() + "/file/tmp/" + fileId + ".part";
}
std::string Dispatcher::fileFinalPath(const std::string& fileId, const std::string& name) const
{
    return m_server.uploadDir() + "/file/" + fileId + "_" + name;
}

void Dispatcher::onFileOfferRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileOfferRq rq;
    if (!parsePayload(payload, rq)) return;

    FileOfferRs rs;
    rs.set_msg_id(rq.msg_id());
    rs.set_file_id(rq.msg_id()); // file_id == msg_id
    if (rq.file_size() > FILE_MAX_SIZE || rq.file_size() < 0) {
        rs.set_result(FILE_OFFER_TOO_LARGE);
        rs.set_received_chunks(0);
        s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_server.uploadDir() + "/file/tmp", ec);
    // 水位线 N = 已有 .part 大小 / CHUNK_SIZE（顺序追加，故整除即已连续收到块数）
    const std::string part = filePartPath(rq.msg_id());
    std::int64_t partSize = 0;
    if (std::filesystem::exists(part, ec)) partSize = (std::int64_t)std::filesystem::file_size(part, ec);
    const int n = (int)(partSize / (std::int64_t)FILE_CHUNK_SIZE);
    rs.set_received_chunks(n);
    rs.set_result(FILE_OFFER_OK);
    s->deliver(DEF_PROT_FILE_OFFER_RS, rs.SerializeAsString());
    log("[业务] 文件协商 id=", s->userId(), " file=", rq.file_name(), " size=", rq.file_size(), " N=", n);
}

void Dispatcher::onFileChunkRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileChunkRq rq;
    if (!parsePayload(payload, rq)) return;

    const std::string part = filePartPath(rq.file_id());
    std::error_code ec;
    std::int64_t partSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;
    const int n = (int)(partSize / (std::int64_t)FILE_CHUNK_SIZE);
    if (rq.chunk_index() == n) {
        std::ofstream ofs(part, std::ios::binary | std::ios::app);
        ofs.write(rq.data().data(), (std::streamsize)rq.data().size());
    } // <n 幂等丢弃；>n 忽略（顺序传输不应乱序）

    // 回进度（新水位线）
    std::int64_t newSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;
    FileProgressRs pr;
    pr.set_file_id(rq.file_id());
    pr.set_received_chunks((int)(newSize / (std::int64_t)FILE_CHUNK_SIZE));
    pr.set_status(FILE_ST_UPLOADING);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
}

void Dispatcher::onFileCompleteRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    const int uid = s->userId();
    if (uid <= 0) return;
    FileCompleteRq rq;
    if (!parsePayload(payload, rq)) return;

    // 从 Offer 缓存不到元数据 —— Complete 只带 file_id/msg_id，需要文件名/size/sha/receiver。
    // 方案：Offer 时把元数据落一张内存表 m_pendingUploads[file_id]。
    auto it = m_pendingUploads.find(rq.file_id());
    if (it == m_pendingUploads.end()) return;
    const PendingUpload up = it->second;

    const std::string part = filePartPath(rq.file_id());
    std::error_code ec;
    std::int64_t partSize = std::filesystem::exists(part, ec)
        ? (std::int64_t)std::filesystem::file_size(part, ec) : 0;

    auto fail = [&]() {
        std::filesystem::remove(part, ec);
        m_pendingUploads.erase(rq.file_id());
        FileProgressRs pr; pr.set_file_id(rq.file_id());
        pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
    };

    if (partSize != up.fileSize) { fail(); return; }
    // 流式 sha256（复用 im::Sha256 增量类）
    im::Sha256 h;
    {
        std::ifstream ifs(part, std::ios::binary);
        std::vector<char> buf(64 * 1024);
        while (ifs) { ifs.read(buf.data(), (std::streamsize)buf.size());
            if (ifs.gcount() > 0) h.update(buf.data(), (std::size_t)ifs.gcount()); }
    }
    const std::vector<unsigned char> dg = h.final();
    static const char* hex = "0123456789abcdef";
    std::string got; got.reserve(64);
    for (unsigned char b : dg) { got.push_back(hex[(b>>4)&0xF]); got.push_back(hex[b&0xF]); }
    if (got != up.sha256) { fail(); return; }

    // 落盘：移动到成品
    const std::string finalPath = fileFinalPath(rq.file_id(), up.fileName);
    std::filesystem::rename(part, finalPath, ec);
    if (ec) { std::filesystem::copy_file(part, finalPath, std::filesystem::copy_options::overwrite_existing, ec);
              std::filesystem::remove(part, ec); }

    // 建文件消息（type=2）
    StoredMessage m;
    m.msgId = rq.msg_id();
    m.senderId = uid;
    m.receiverId = up.receiverId;
    m.type = 2;
    m.content = up.fileName;       // content 存文件名
    m.mediaPath = finalPath;
    m.fileId = rq.file_id();
    m.fileSize = up.fileSize;
    m.ts = nowSec();
    const bool online = (bool)m_server.presence().get(up.receiverId);
    m_server.db().saveMessage(m, online);

    // 组文件卡片（ChatInfoRq type=FILE），转发在线接收方 / 离线等补发
    ChatInfoRq card;
    card.set_myid(uid);
    card.set_friid(up.receiverId);
    card.set_type(FILE);
    card.set_msg_id(rq.msg_id());
    card.set_ts(m.ts);
    card.set_seq(m.seq);
    card.set_file_name(up.fileName);
    card.set_file_size(up.fileSize);
    card.set_file_id(rq.file_id());
    if (auto target = m_server.presence().get(up.receiverId)) {
        target->deliver(DEF_PROT_CHAT_INFO_RQ, card.SerializeAsString());
    }

    m_pendingUploads.erase(rq.file_id());
    FileProgressRs pr; pr.set_file_id(rq.file_id());
    pr.set_status(FILE_ST_DONE);
    pr.set_received_chunks(up.totalChunks); pr.set_total_chunks(up.totalChunks);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
    log("[业务] 文件完成 file=", up.fileName, " -> ", finalPath, " online=", online);
}
```

- [ ] **Step 7: `Dispatcher.h` 加 pending 上传元数据表**

在 private 区加：

```cpp
    struct PendingUpload {
        int receiverId = 0;
        std::string fileName;
        std::int64_t fileSize = 0;
        int totalChunks = 0;
        std::string sha256;
    };
    std::unordered_map<std::string, PendingUpload> m_pendingUploads; // key=file_id
```

并在 `onFileOfferRq` 的 `FILE_OFFER_OK` 分支（发送 Rs 前）写入：

```cpp
    m_pendingUploads[rq.msg_id()] = PendingUpload{
        rq.receiver_id(), rq.file_name(), rq.file_size(), rq.total_chunks(), rq.sha256() };
```

> 说明：`m_pendingUploads` 由业务 strand 访问；不同 file_id 落在各自会话 strand。跨会话并发写同一 map 有竞争风险——本项目单进程 demo，且实际每个 file_id 只由一个发送方会话操作。若需严格，用 `std::mutex` 保护（实现时加一把 `m_uploadMtx`）。

- [ ] **Step 8: `Server::start()` 建 `file/tmp` 目录**

在 `Server.cpp` 的 `create_directories(m_uploadDir + "/file", ec);` 之后加：

```cpp
    std::filesystem::create_directories(m_uploadDir + "/file/tmp", ec);
```

- [ ] **Step 9: 校正 e2e 接收方在线断言 + 运行至通过**

在 Step 2 的 M7 段末尾追加（用当前在线的 b3=李四 收卡片；若 e2e 中此时无在线接收方，改为让接收方先登录）：

```cpp
    // b3(李四, idB) 在线，应收到文件卡片
    assert(eb3.waitFor([&] {
        for (const auto& c : eb3.chats) { /* 文本回调不含文件；文件卡片需专门回调 */ }
        return true; // 见下：ClientCore 收 type=FILE 卡片需在 onChatInfoRq 分流
    }));
```

修正：`ClientCore::onChatInfoRq` 现只分 IMAGE/TEXT。加 `FILE` 分支回调（Task 3 已建的事件里补一个 `onFileCard`）。**在 Task 3 的 IClientEvents 追加**：

```cpp
    virtual void onFileCard(int fromId, const std::string& fileId, const std::string& name,
                            std::int64_t size, const std::string& msgId) { (void)fromId;(void)fileId;(void)name;(void)size;(void)msgId; }
```

并在 `ClientCore::onChatInfoRq` 的 IMAGE 分支前加：

```cpp
    if (rq.type() == im::proto::FILE) {
        if (auto* ev = m_events.load())
            ev->onFileCard(rq.myid(), rq.file_id(), rq.file_name(), rq.file_size(), rq.msg_id());
        return;
    }
```

e2e `RecordingEvents` 加 `std::vector<...> fileCards;` 与 override，断言收到卡片：

```cpp
    assert(eb3.waitFor([&] {
        for (auto& fc : eb3.fileCards) if (fc.fileId == fmsgId && fc.name == "report.bin") return true;
        return false;
    }));
```

Run: `cmake --build build --target test_e2e && ./build/test_e2e`
Expected: `test_e2e PASSED`（上传闭环 + 卡片转发通过；M1~M6 回归通过）。

- [ ] **Step 10: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/src/Dispatcher.h IMproject-main/im_server/src/Dispatcher.cpp IMproject-main/im_server/src/Server.cpp IMproject-main/im_server/tests/test_e2e.cpp IMproject-main/client_core/include/client_core/ClientCore.h IMproject-main/client_core/src/ClientCore.cpp
git commit -m "M7-4: 服务端文件上传(Offer/Chunk/Complete)+卡片转发 + e2e 上传闭环"
```

---

### Task 5: e2e 断点续传（重发 Offer 返回 N>0）

**Files:**
- Modify: `im_server/tests/test_e2e.cpp`（追加续传段）

**Interfaces:** Consumes Task 4 全部。

- [ ] **Step 1: 追加续传断言（在 M7 上传段之后）**

```cpp
    // 断点续传：新文件只发第 0 块就"中断"，重发 Offer 应返回 N=1
    const std::string fmsgId2 = "file-e2e-0002";
    std::string big(300 * 1024, 'A');
    const std::string big2Sha = im::sha256Hex(big);
    a2.sendFileOffer(fmsgId2, idB, "resume.bin", (std::int64_t)big.size(), 2, big2Sha);
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId2; }));
    a2.sendFileChunk(fmsgId2, 0, big.substr(0, 256*1024)); // 只发第 0 块
    // 等服务端落盘该块
    assert(ea2.waitFor([&] { return ea2.lastProgressRecv >= 1; }));
    ea2.gotOffer = false;
    a2.sendFileOffer(fmsgId2, idB, "resume.bin", (std::int64_t)big.size(), 2, big2Sha); // 重发 Offer
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId2; }));
    assert(ea2.lastOffer.receivedChunks == 1); // 水位线续传起点=1
    a2.sendFileChunk(fmsgId2, 1, big.substr(256*1024)); // 续发第 1 块
    a2.sendFileComplete(fmsgId2, fmsgId2);
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_DONE; }));
```

- [ ] **Step 2: 运行至通过**

Run: `cmake --build build --target test_e2e && ./build/test_e2e`
Expected: `test_e2e PASSED`。

- [ ] **Step 3: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/tests/test_e2e.cpp
git commit -m "M7-5: e2e 断点续传（重发 Offer 返回水位线 N>0）"
```

---

### Task 6: Dispatcher 下载（onFileDownloadRq）+ e2e 下载续传

**Files:**
- Modify: `im_server/src/Dispatcher.cpp`（`onFileDownloadRq` 实现）
- Modify: `im_server/tests/test_e2e.cpp`（下载段）

**Interfaces:** Consumes Task 2 `getMessageByMsgId`、Task 4 成品文件。

- [ ] **Step 1: e2e 下载断言（先失败）——在续传段之后**

```cpp
    // 下载：b3(李四) 从 file-e2e-0001 的第 0 块开始拉，应收到 2 块，拼接 == fileBytes
    b3? // 用当前在线接收方 session（下称 b3）
    eb3.downloadChunks.clear();
    b3.sendFileDownload(fmsgId, 0);
    assert(eb3.waitFor([&] {
        int bytes = 0; for (auto& c : eb3.downloadChunks) bytes += (int)c.second.size();
        return bytes == (int)fileBytes.size();
    }));
    // 断点续传下载：从第 1 块拉，只应收到第 1 块（44KB）
    eb3.downloadChunks.clear();
    b3.sendFileDownload(fmsgId, 1);
    assert(eb3.waitFor([&] {
        return eb3.downloadChunks.size() == 1 && eb3.downloadChunks[0].first == 1;
    }));
```

- [ ] **Step 2: 实现 `onFileDownloadRq`（在 `onFileCompleteRq` 之后）**

```cpp
void Dispatcher::onFileDownloadRq(const std::shared_ptr<Session>& s, const std::string& payload)
{
    if (s->userId() <= 0) return;
    FileDownloadRq rq;
    if (!parsePayload(payload, rq)) return;

    StoredMessage m;
    if (!m_server.db().getMessageByMsgId(rq.file_id(), m) || m.type != 2 || m.mediaPath.empty()) {
        FileProgressRs pr; pr.set_file_id(rq.file_id()); pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    std::ifstream ifs(m.mediaPath, std::ios::binary);
    if (!ifs) {
        FileProgressRs pr; pr.set_file_id(rq.file_id()); pr.set_status(FILE_ST_FAILED);
        s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
        return;
    }
    const int total = (int)((m.fileSize + (std::int64_t)FILE_CHUNK_SIZE - 1) / (std::int64_t)FILE_CHUNK_SIZE);
    int idx = rq.from_chunk() < 0 ? 0 : rq.from_chunk();
    ifs.seekg((std::streamoff)idx * (std::streamoff)FILE_CHUNK_SIZE, std::ios::beg);
    std::vector<char> buf(FILE_CHUNK_SIZE);
    for (; idx < total; ++idx) {
        ifs.read(buf.data(), (std::streamsize)FILE_CHUNK_SIZE);
        std::streamsize got = ifs.gcount();
        if (got <= 0) break;
        FileChunkRq ch;
        ch.set_file_id(rq.file_id());
        ch.set_chunk_index(idx);
        ch.set_data(std::string(buf.data(), (size_t)got));
        s->deliver(DEF_PROT_FILE_CHUNK_RQ, ch.SerializeAsString());
    }
    FileProgressRs pr; pr.set_file_id(rq.file_id());
    pr.set_status(FILE_ST_DONE); pr.set_total_chunks(total); pr.set_received_chunks(total);
    s->deliver(DEF_PROT_FILE_PROGRESS_RS, pr.SerializeAsString());
}
```

- [ ] **Step 3: 运行至通过**

Run: `cmake --build build --target test_e2e && ./build/test_e2e`
Expected: `test_e2e PASSED`。

- [ ] **Step 4: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/src/Dispatcher.cpp IMproject-main/im_server/tests/test_e2e.cpp
git commit -m "M7-6: 服务端下载(FileDownloadRq seek 分片) + e2e 下载续传"
```

---

### Task 7: e2e 边界（超限拒绝 + sha256 不符 failed）

**Files:** Modify `im_server/tests/test_e2e.cpp`

- [ ] **Step 1: 追加边界断言**

```cpp
    // 超限：声明 101MB → 拒绝
    ea2.gotOffer = false;
    a2.sendFileOffer("file-toolarge", idB, "big.bin", 101LL*1024*1024, 999, "deadbeef");
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == "file-toolarge"; }));
    assert(ea2.lastOffer.result == FILE_OFFER_TOO_LARGE);

    // sha256 不符：发正确字节但 Offer 声明错误 sha → Complete 应 failed
    const std::string fmsgId3 = "file-badsha";
    std::string data3(1024, 'Z');
    ea2.gotOffer = false; ea2.lastProgressStatus = -1;
    a2.sendFileOffer(fmsgId3, idB, "bad.bin", (std::int64_t)data3.size(), 1, "0000000000000000000000000000000000000000000000000000000000000000");
    assert(ea2.waitFor([&] { return ea2.gotOffer && ea2.lastOffer.msgId == fmsgId3; }));
    a2.sendFileChunk(fmsgId3, 0, data3);
    a2.sendFileComplete(fmsgId3, fmsgId3);
    assert(ea2.waitFor([&] { return ea2.lastProgressStatus == FILE_ST_FAILED; }));
```

- [ ] **Step 2: 运行至通过**

Run: `cmake --build build --target test_e2e && ./build/test_e2e`
Expected: `test_e2e PASSED`。

- [ ] **Step 3: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/tests/test_e2e.cpp
git commit -m "M7-7: e2e 边界（超 100MB 拒绝 + sha256 不符 failed）"
```

---

### Task 8: Server 启动清理超时 .part

**Files:** Modify `im_server/src/Server.cpp`（`start()` 里清理）

- [ ] **Step 1: 在 `Server::start()` 建目录之后加清理逻辑**

```cpp
    // 清理超 24h 未完成的半成品 .part（断点续传窗口外）
    {
        std::error_code ec;
        const auto tmpDir = std::filesystem::path(m_uploadDir) / "file" / "tmp";
        if (std::filesystem::exists(tmpDir, ec)) {
            const auto now = std::filesystem::file_time_type::clock::now();
            for (auto& e : std::filesystem::directory_iterator(tmpDir, ec)) {
                if (!e.is_regular_file()) continue;
                auto mtime = std::filesystem::last_write_time(e, ec);
                if (!ec && (now - mtime) > std::chrono::hours(24))
                    std::filesystem::remove(e.path(), ec);
            }
        }
    }
```

（`<chrono>` 已在 Server.cpp include。）

- [ ] **Step 2: 编译 + e2e 回归**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && cmake --build build --target test_e2e && ./build/test_e2e`
Expected: `test_e2e PASSED`。

- [ ] **Step 3: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/im_server/src/Server.cpp
git commit -m "M7-8: 服务端启动清理超 24h 未完成 .part"
```

---

### Task 9: Android Room 迁移（v2→v3）+ Entities/Store 文件字段

**Files:**
- Modify: `.../data/db/Entities.kt`（`MessageEntity` 加字段）
- Modify: `.../data/db/AppDatabase.kt`（version=3 + MIGRATION_2_3）
- Modify: `.../data/ChatStore.kt`（`toEntity`/`toChatMessage` 映射 + `save` 支持文件）
- Modify: `.../ui/MainViewModel.kt`（`ChatMessage`/`MsgKind` 加 FILE + 文件字段）

**Interfaces:**
- Produces：`MsgKind.FILE`；`ChatMessage` 加 `fileId:String=""`, `fileName:String=""`, `fileSize:Long=0`, `localPath:String?=null`, `transferred:Int=0`；`MessageEntity` 同名列。

- [ ] **Step 1: `Entities.kt` 的 `MessageEntity` 追加字段**

在 `val status: Int,` 之前加：

```kotlin
    val fileId: String = "",
    val fileName: String = "",
    val fileSize: Long = 0,
    val localPath: String? = null,   // 文件本地路径（.part 进行中 / 成品）
    val transferred: Int = 0,        // 已传/已收块数（进度 + 续传游标）
```

- [ ] **Step 2: `AppDatabase.kt` 升版本 + 迁移**

将 `version = 2` 改为 `version = 3`。在 `AppDatabase` 的 companion（`get()` 构建 Room 处）加 migration。找到 `Room.databaseBuilder(...)` 链，加 `.addMigrations(MIGRATION_2_3)`，并定义：

```kotlin
val MIGRATION_2_3 = object : androidx.room.migration.Migration(2, 3) {
    override fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase) {
        db.execSQL("ALTER TABLE messages ADD COLUMN fileId TEXT NOT NULL DEFAULT ''")
        db.execSQL("ALTER TABLE messages ADD COLUMN fileName TEXT NOT NULL DEFAULT ''")
        db.execSQL("ALTER TABLE messages ADD COLUMN fileSize INTEGER NOT NULL DEFAULT 0")
        db.execSQL("ALTER TABLE messages ADD COLUMN localPath TEXT")
        db.execSQL("ALTER TABLE messages ADD COLUMN transferred INTEGER NOT NULL DEFAULT 0")
    }
}
```

- [ ] **Step 3: `MainViewModel.kt` 的 `MsgKind`/`ChatMessage` 扩展**

`enum class MsgKind { TEXT, IMAGE, FILE }`。`ChatMessage` 加：

```kotlin
    val fileId: String = "",
    val fileName: String = "",
    val fileSize: Long = 0,
    val localPath: String? = null,
    val transferred: Int = 0,
```

- [ ] **Step 4: `ChatStore.kt` 映射文件字段**

`toEntity`：加 `fileId=fileId, fileName=fileName, fileSize=fileSize, localPath=localPath, transferred=transferred`，且 `type = when(kind){IMAGE->1; FILE->2; else->0}`。
`toChatMessage`：`kind = when(type){1->IMAGE;2->FILE;else->TEXT}`，回填 `fileId/fileName/fileSize/localPath/transferred`。
`save` 的 `lastMsg`：`FILE -> "[文件] $fileName"`。

- [ ] **Step 5: 编译 Android**

Run:
```bash
cd "/Users/xiaozong/zzq/IM Project/IMproject-main/jitong_android"
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
GRADLE="$(echo ~/.gradle/wrapper/dists/gradle-8.9-bin/*/gradle-8.9/bin/gradle)"
"$GRADLE" :app:compileDebugKotlin --console=plain
```
Expected: `BUILD SUCCESSFUL`。

- [ ] **Step 6: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/jitong_android/app/src/main/java/com/jitong/im/data/db/Entities.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/data/db/AppDatabase.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/data/ChatStore.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/ui/MainViewModel.kt
git commit -m "M7-9: Android Room v3 迁移 + 文件字段（Entity/Store/ChatMessage）"
```

---

### Task 10: Android ImClient —— 文件收发 + Events + 流式 sha256

**Files:**
- Modify: `.../net/Sha256.kt`（加 `sha256HexOfStream`）
- Modify: `.../net/ImClient.kt`（发送方法 + dispatch + Event + FILE 卡片分支）

**Interfaces:**
- Produces：`ImClient` 方法 `suspend fun fileOffer(...)/fileChunk(...)/fileComplete(...)/fileDownload(...)`；Event `FileOfferResult/FileChunk/FileProgress/FileCard`。

- [ ] **Step 1: `Sha256.kt` 加流式变体**

```kotlin
/** 大文件流式 sha256（小写 hex），避免整文件入内存 */
fun sha256HexOfStream(input: java.io.InputStream): String {
    val md = java.security.MessageDigest.getInstance("SHA-256")
    val buf = ByteArray(64 * 1024)
    while (true) {
        val n = input.read(buf); if (n < 0) break
        md.update(buf, 0, n)
    }
    return md.digest().joinToString("") { "%02x".format(it) }
}
```

- [ ] **Step 2: `ImClient.kt` 加 Event（在 `RoamMessages` 之后）**

```kotlin
        data class FileOfferResult(val msgId: String, val fileId: String, val receivedChunks: Int, val result: Int) : Event
        data class FileChunk(val fileId: String, val chunkIndex: Int, val data: ByteArray) : Event
        data class FileProgress(val fileId: String, val received: Int, val total: Int, val status: Int) : Event
        data class FileCard(val fromId: Int, val fileId: String, val name: String, val size: Long, val msgId: String, val ts: Long, val seq: Long) : Event
```

- [ ] **Step 3: `ImClient.kt` 加发送方法（在 `roamMessages` 之后）**

```kotlin
    suspend fun fileOffer(msgId: String, receiverId: Int, name: String, size: Long, totalChunks: Int, sha256: String) {
        val rq = Im.FileOfferRq.newBuilder()
            .setMsgId(msgId).setReceiverId(receiverId).setFileName(name)
            .setFileSize(size).setTotalChunks(totalChunks).setSha256(sha256).build()
        send(Protocol.FILE_OFFER_RQ, rq.toByteArray())
    }
    suspend fun fileChunk(fileId: String, index: Int, data: ByteArray) {
        val rq = Im.FileChunkRq.newBuilder()
            .setFileId(fileId).setChunkIndex(index)
            .setData(com.google.protobuf.ByteString.copyFrom(data)).build()
        send(Protocol.FILE_CHUNK_RQ, rq.toByteArray())
    }
    suspend fun fileComplete(fileId: String, msgId: String) {
        val rq = Im.FileCompleteRq.newBuilder().setFileId(fileId).setMsgId(msgId).build()
        send(Protocol.FILE_COMPLETE_RQ, rq.toByteArray())
    }
    suspend fun fileDownload(fileId: String, fromChunk: Int) {
        val rq = Im.FileDownloadRq.newBuilder().setFileId(fileId).setFromChunk(fromChunk).build()
        send(Protocol.FILE_DOWNLOAD_RQ, rq.toByteArray())
    }
```

- [ ] **Step 4: `ImClient.kt` dispatch 新分支（在 `ROAM_MSG_RS` 之后）**

```kotlin
            Protocol.FILE_OFFER_RS -> {
                val rs = Im.FileOfferRs.parseFrom(f.payload)
                _events.emit(Event.FileOfferResult(rs.msgId, rs.fileId, rs.receivedChunks, rs.result))
            }
            Protocol.FILE_CHUNK_RQ -> {
                val rq = Im.FileChunkRq.parseFrom(f.payload)
                _events.emit(Event.FileChunk(rq.fileId, rq.chunkIndex, rq.data.toByteArray()))
            }
            Protocol.FILE_PROGRESS_RS -> {
                val rs = Im.FileProgressRs.parseFrom(f.payload)
                _events.emit(Event.FileProgress(rs.fileId, rs.receivedChunks, rs.totalChunks, rs.status))
            }
```

- [ ] **Step 5: `ImClient.kt` 的 `dispatch` 中 `CHAT_INFO_RQ` 分支加 FILE 卡片**

在现有 `when (rq.type)` 的 `Im.MsgType.IMAGE ->` 之后加：

```kotlin
                    Im.MsgType.FILE ->
                        _events.emit(Event.FileCard(rq.myid, rq.fileId, rq.fileName, rq.fileSize, rq.msgId, tsMs, rq.seq))
```

- [ ] **Step 6: 编译 Android**

Run:（同 Task 9 Step 5）`"$GRADLE" :app:compileDebugKotlin --console=plain`
Expected: `BUILD SUCCESSFUL`。

- [ ] **Step 7: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/jitong_android/app/src/main/java/com/jitong/im/net/Sha256.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/net/ImClient.kt
git commit -m "M7-10: Android ImClient 文件收发 + Event + 流式 sha256"
```

---

### Task 11: Android MainViewModel —— 发送方状态机 + 断点续传

**Files:** Modify `.../ui/MainViewModel.kt`

**Interfaces:**
- Consumes: Task 10 `ImClient` 文件方法/事件、Task 9 `ChatMessage` 文件字段、`ChatStore`。
- Produces: `fun sendFile(uri: Uri)`（供 ChatScreen SAF 回调）、事件落地。

- [ ] **Step 1: 加上传态与 sendFile**

在 `MainViewModel` 加字段与方法（`context` 通过 `attachStore` 已有的 appContext 或新注入；这里用 `ChatStore` 持有的 appContext，通过新增 `store.appContext()` 暴露，或在 sendFile 传 ContentResolver）。实现：

```kotlin
    // 进行中的上传：fileId -> 待发字节源信息
    private data class Upload(val uri: Uri, val peerId: Int, val name: String, val size: Long, val totalChunks: Int)
    private val uploads = mutableMapOf<String, Upload>()

    fun sendFile(uri: Uri, resolver: android.content.ContentResolver) {
        val peer = _chatPeer.value ?: return
        val (name, size) = queryNameSize(resolver, uri) ?: return
        if (size > Protocol.FILE_MAX_SIZE) { notify("文件超过 100MB"); return }
        val msgId = java.util.UUID.randomUUID().toString()
        val totalChunks = ((size + Protocol.FILE_CHUNK_SIZE - 1) / Protocol.FILE_CHUNK_SIZE).toInt()
        uploads[msgId] = Upload(uri, peer.id, name, size, totalChunks)
        append(ChatMessage(msgId, peer.id, fromMe = true, kind = MsgKind.FILE,
            fileId = msgId, fileName = name, fileSize = size, localPath = uri.toString(),
            status = ChatMessage.Status.SENDING), incrUnread = false)
        viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val sha = resolver.openInputStream(uri)!!.use { com.jitong.im.net.sha256HexOfStream(it) }
            client.fileOffer(msgId, peer.id, name, size, totalChunks, sha)
        }
    }

    private fun queryNameSize(resolver: android.content.ContentResolver, uri: Uri): Pair<String, Long>? {
        resolver.query(uri, null, null, null, null)?.use { c ->
            val ni = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
            val si = c.getColumnIndex(android.provider.OpenableColumns.SIZE)
            if (c.moveToFirst()) {
                val name = if (ni >= 0) c.getString(ni) else "file"
                val size = if (si >= 0) c.getLong(si) else -1L
                if (size >= 0) return name to size
            }
        }
        return null
    }
```

- [ ] **Step 2: 事件落地 —— FileOfferResult 触发发块**

在 `collectEvents()` 的 when 里加：

```kotlin
                is ImClient.Event.FileOfferResult -> {
                    val up = uploads[e.msgId] ?: return@collect
                    if (e.result != Protocol.FILE_OFFER_OK) {
                        updateFileStatus(up.peerId, e.msgId, ChatMessage.Status.OFFLINE_STORED) // 复用"失败"展示
                        notify("文件被拒绝（超限）"); uploads.remove(e.msgId); return@collect
                    }
                    val resolver = appResolver ?: return@collect
                    viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                        resolver.openInputStream(up.uri)!!.use { ins ->
                            var idx = 0
                            val buf = ByteArray(Protocol.FILE_CHUNK_SIZE)
                            // 跳过已发送的 e.receivedChunks 块（断点续传）
                            var skip = e.receivedChunks.toLong() * Protocol.FILE_CHUNK_SIZE
                            while (skip > 0) { val s = ins.skip(skip); if (s <= 0) break; skip -= s }
                            idx = e.receivedChunks
                            while (true) {
                                val n = ins.read(buf); if (n < 0) break
                                client.fileChunk(e.fileId, idx, buf.copyOf(n)); idx++
                            }
                            client.fileComplete(e.fileId, e.msgId)
                        }
                    }
                }
                is ImClient.Event.FileProgress -> {
                    val up = uploads[e.fileId]
                    if (up != null) {
                        if (e.status == Protocol.FILE_ST_DONE) {
                            updateFileStatus(up.peerId, e.fileId, ChatMessage.Status.DELIVERED)
                            uploads.remove(e.fileId)
                        } else if (e.status == Protocol.FILE_ST_FAILED) {
                            notify("文件发送失败"); uploads.remove(e.fileId)
                        } else {
                            updateFileProgress(up.peerId, e.fileId, e.received)
                        }
                    } else {
                        // 下载进度（接收方），见 Task 12
                        onDownloadProgress(e)
                    }
                }
```

- [ ] **Step 3: 加辅助方法 + appResolver 注入**

```kotlin
    @Volatile private var appResolver: android.content.ContentResolver? = null
    fun attachResolver(r: android.content.ContentResolver) { appResolver = r }

    private fun updateFileStatus(peerId: Int, msgId: String, status: ChatMessage.Status) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(status = status) else it })
        viewModelScope.launch { store?.updateStatus(client.myId, msgId, status) }
    }
    private fun updateFileProgress(peerId: Int, msgId: String, transferred: Int) {
        val conv = _messages.value[peerId] ?: return
        _messages.value = _messages.value + (peerId to conv.map {
            if (it.msgId == msgId) it.copy(transferred = transferred) else it })
    }
```

（`MainActivity` 在 `attachStore` 处一并 `vm.attachResolver(contentResolver)`。）

- [ ] **Step 4: 编译**

Run:（同前）`"$GRADLE" :app:compileDebugKotlin --console=plain`
Expected: `BUILD SUCCESSFUL`（`onDownloadProgress` 在 Task 12 定义前先加空桩 `private fun onDownloadProgress(e: ImClient.Event.FileProgress) {}`）。

- [ ] **Step 5: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/jitong_android/app/src/main/java/com/jitong/im/ui/MainViewModel.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/MainActivity.kt
git commit -m "M7-11: Android 发送方状态机 + 断点续传（skip 已发块续发）"
```

---

### Task 12: Android MainViewModel —— 接收方卡片/下载 + 续传

**Files:** Modify `.../ui/MainViewModel.kt`, `.../data/ChatStore.kt`

**Interfaces:** Consumes Task 10 `FileCard/FileChunk/FileProgress`。Produces `fun downloadFile(msg: ChatMessage)`。

- [ ] **Step 1: FileCard 落地（收到卡片插消息）**

在 `collectEvents()` 加：

```kotlin
                is ImClient.Event.FileCard -> {
                    val inChat = _screen.value == Screen.Chat && _chatPeer.value?.id == e.fromId
                    append(ChatMessage(e.msgId, e.fromId, fromMe = false, kind = MsgKind.FILE,
                        fileId = e.fileId, fileName = e.name, fileSize = e.size, ts = e.ts, seq = e.seq,
                        status = ChatMessage.Status.RECEIVED), incrUnread = !inChat)
                }
```

- [ ] **Step 2: 下载状态 + downloadFile**

```kotlin
    private data class Download(val peerId: Int, val fileId: String, val name: String, val size: Long,
                                val out: java.io.OutputStream, val partFile: java.io.File)
    private val downloads = mutableMapOf<String, Download>()

    fun downloadFile(msg: ChatMessage) {
        val ctx = appContext ?: return
        val dir = java.io.File(ctx.filesDir, "file").apply { mkdirs() }
        val part = java.io.File(dir, "${msg.fileId}_${msg.fileName}.part")
        val fromChunk = (part.length() / Protocol.FILE_CHUNK_SIZE).toInt()
        val out = java.io.FileOutputStream(part, /*append=*/true)
        downloads[msg.fileId] = Download(msg.peerId, msg.fileId, msg.fileName, msg.fileSize, out, part)
        updateFileStatus(msg.peerId, msg.msgId, ChatMessage.Status.SENDING) // 复用"进行中"
        viewModelScope.launch { client.fileDownload(msg.fileId, fromChunk) }
    }

    // FileChunk 落地（追加写 .part）
    // 在 collectEvents 加：
                is ImClient.Event.FileChunk -> {
                    val d = downloads[e.fileId] ?: return@collect
                    withContext(kotlinx.coroutines.Dispatchers.IO) { d.out.write(e.data); d.out.flush() }
                    val chunks = (d.partFile.length() / Protocol.FILE_CHUNK_SIZE).toInt()
                    updateFileProgress(d.peerId, e.fileId, chunks)
                }
```

- [ ] **Step 3: 用实际下载逻辑替换 Task 11 的 `onDownloadProgress` 空桩**

```kotlin
    private fun onDownloadProgress(e: ImClient.Event.FileProgress) {
        val d = downloads[e.fileId] ?: return
        if (e.status == Protocol.FILE_ST_DONE) {
            viewModelScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                runCatching { d.out.close() }
                val ctx = appContext ?: return@launch
                val finalFile = java.io.File(java.io.File(ctx.filesDir, "file"), "${d.fileId}_${d.name}")
                d.partFile.renameTo(finalFile)
                store?.updateFileLocalPath(client.myId, d.fileId, finalFile.absolutePath)
                withContext(kotlinx.coroutines.Dispatchers.Main) {
                    val conv = _messages.value[d.peerId] ?: return@withContext
                    _messages.value = _messages.value + (d.peerId to conv.map {
                        if (it.fileId == d.fileId) it.copy(localPath = finalFile.absolutePath, status = ChatMessage.Status.RECEIVED) else it })
                }
                downloads.remove(d.fileId)
            }
        } else if (e.status == Protocol.FILE_ST_FAILED) {
            runCatching { d.out.close() }; downloads.remove(e.fileId); notify("文件已失效")
        }
    }
```

- [ ] **Step 4: `ChatStore.kt` 加 `updateFileLocalPath` + Daos**

`Daos.kt` `MessageDao` 加：

```kotlin
    @Query("UPDATE messages SET localPath = :path WHERE ownerId = :ownerId AND fileId = :fileId")
    suspend fun updateLocalPath(ownerId: Int, fileId: String, path: String)
```

`ChatStore.kt` 加：

```kotlin
    suspend fun updateFileLocalPath(ownerId: Int, fileId: String, path: String) =
        withContext(Dispatchers.IO) { db.messageDao().updateLocalPath(ownerId, fileId, path) }
```

`MainViewModel` 需要 `appContext`：在 `attachStore` 里保存 `this.appContext = store 的 appContext`（给 `ChatStore` 加 `fun context(): Context = appContext` 暴露），或 `attachResolver` 时一并存 `Context`。实现：给 MainViewModel 加 `@Volatile private var appContext: android.content.Context? = null`，`MainActivity` 调 `vm.attachContext(applicationContext)`。

- [ ] **Step 5: 编译**

Run:（同前）`"$GRADLE" :app:compileDebugKotlin --console=plain`
Expected: `BUILD SUCCESSFUL`。

- [ ] **Step 6: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/jitong_android/app/src/main/java/com/jitong/im/ui/MainViewModel.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/data/ChatStore.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/data/db/Daos.kt IMproject-main/jitong_android/app/src/main/java/com/jitong/im/MainActivity.kt
git commit -m "M7-12: Android 接收方卡片/下载 + 断点续传(.part 追加)"
```

---

### Task 13: Android ChatScreen —— SAF 选文件 + 文件卡片 UI + FileProvider 打开

**Files:**
- Modify: `.../ui/ChatScreen.kt`（SAF launcher + 文件卡片 composable）
- Modify: `.../AndroidManifest.xml`（FileProvider）
- Create: `.../app/src/main/res/xml/file_paths.xml`

**Interfaces:** Consumes Task 11 `sendFile`、Task 12 `downloadFile`、`ChatMessage(kind=FILE)`。

- [ ] **Step 1: `res/xml/file_paths.xml`（新建）**

```xml
<?xml version="1.0" encoding="utf-8"?>
<paths>
    <files-path name="files" path="file/" />
</paths>
```

- [ ] **Step 2: `AndroidManifest.xml` 加 FileProvider（`<application>` 内）**

```xml
        <provider
            android:name="androidx.core.content.FileProvider"
            android:authorities="com.jitong.im.fileprovider"
            android:exported="false"
            android:grantUriPermissions="true">
            <meta-data
                android:name="android.support.FILE_PROVIDER_PATHS"
                android:resource="@xml/file_paths" />
        </provider>
```

（确认 `app/build.gradle.kts` 依赖含 `androidx.core:core-ktx`，`FileProvider` 在其中。）

- [ ] **Step 3: `ChatScreen.kt` 加 SAF 选文件 launcher**

在 `pickImage` launcher 之后加：

```kotlin
    val pickFile = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) vm.sendFile(uri, context.contentResolver)
    }
```

把"+面板"里 `PanelItem("文件")` 的 onClick 改为：

```kotlin
                    PanelItem("文件") {
                        showPanel = false
                        pickFile.launch(arrayOf("*/*"))
                    }
```

- [ ] **Step 4: `ChatScreen.kt` 的 `MessageRow` 里 `when (msg.kind)` 加 FILE 分支**

```kotlin
                MsgKind.FILE -> FileBubble(msg, onDownload = { vm.downloadFile(msg) }, onOpen = {
                    msg.localPath?.let { openFile(context, it, msg.fileName) }
                })
```

- [ ] **Step 5: 加 `FileBubble` composable + `openFile`**

```kotlin
@Composable
private fun FileBubble(msg: ChatMessage, onDownload: () -> Unit, onOpen: () -> Unit) {
    val downloaded = msg.localPath != null && !msg.localPath!!.endsWith(".part")
    Column(
        Modifier.widthIn(max = 260.dp)
            .background(if (msg.fromMe) Color(0xFF95EC69) else Color.White, RoundedCornerShape(8.dp))
            .clickable { if (downloaded) onOpen() else if (!msg.fromMe) onDownload() }
            .padding(12.dp),
    ) {
        Text("📄 ${msg.fileName}", fontSize = 14.sp)
        Text(humanSize(msg.fileSize), fontSize = 11.sp, color = Color.Gray)
        val total = ((msg.fileSize + FILE_CHUNK - 1) / FILE_CHUNK).toInt().coerceAtLeast(1)
        when {
            msg.status == ChatMessage.Status.SENDING ->
                Text("${(msg.transferred * 100 / total)}%", fontSize = 11.sp, color = Color.Gray)
            downloaded -> Text("点击打开", fontSize = 11.sp, color = Color(0xFF07C160))
            !msg.fromMe -> Text("点击下载", fontSize = 11.sp, color = Color(0xFF07C160))
            else -> Text("已发送", fontSize = 11.sp, color = Color.Gray)
        }
    }
}

private const val FILE_CHUNK = 256 * 1024
private fun humanSize(b: Long): String = when {
    b >= 1 shl 20 -> "%.1f MB".format(b / 1048576.0)
    b >= 1 shl 10 -> "%.1f KB".format(b / 1024.0)
    else -> "$b B"
}
private fun openFile(context: android.content.Context, path: String, name: String) {
    val file = java.io.File(path)
    val uri = androidx.core.content.FileProvider.getUriForFile(context, "com.jitong.im.fileprovider", file)
    val mime = context.contentResolver.getType(uri) ?: "*/*"
    val intent = android.content.Intent(android.content.Intent.ACTION_VIEW)
        .setDataAndType(uri, mime)
        .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
    runCatching { context.startActivity(intent) }
}
```

（顶部需 `import androidx.compose.foundation.clickable`（已存在）。）

- [ ] **Step 6: 编译 + 打包 APK**

Run:
```bash
cd "/Users/xiaozong/zzq/IM Project/IMproject-main/jitong_android"
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
GRADLE="$(echo ~/.gradle/wrapper/dists/gradle-8.9-bin/*/gradle-8.9/bin/gradle)"
"$GRADLE" :app:assembleDebug --console=plain
```
Expected: `BUILD SUCCESSFUL`，产物 `app/build/outputs/apk/debug/app-debug.apk`。

- [ ] **Step 7: Commit**

```bash
cd "/Users/xiaozong/zzq/IM Project"
git add IMproject-main/jitong_android/app/src/main/java/com/jitong/im/ui/ChatScreen.kt IMproject-main/jitong_android/app/src/main/AndroidManifest.xml IMproject-main/jitong_android/app/src/main/res/xml/file_paths.xml
git commit -m "M7-13: Android 文件卡片 UI + SAF 选文件 + FileProvider 打开"
```

---

### Task 14: 端到端手测验证

**Files:** 无（验证）

- [ ] **Step 1: 起服务端**

Run: `"/Users/xiaozong/zzq/IM Project/IMproject-main/im_server/build/im_server"`（后台运行，观察 `[业务] 文件...` 日志）

- [ ] **Step 2: 装 APK 到两个模拟器**

```bash
ADB=~/Library/Android/sdk/platform-tools/adb
APK="/Users/xiaozong/zzq/IM Project/IMproject-main/jitong_android/app/build/outputs/apk/debug/app-debug.apk"
$ADB -s emulator-5554 install -r "$APK"
$ADB -s emulator-5556 install -r "$APK"
```

- [ ] **Step 3: 手测清单（逐项确认）**

1. 5554 张三登录、5556 李四登录。
2. 张三 +面板→文件→选一个 >256KB 文件发送 → 卡片显示进度→已发送；服务端日志 `[业务] 文件完成`。
3. 李四收到文件卡片（名/大小/"点击下载"）。
4. 李四点下载 → 进度→"点击打开" → 点开经系统应用打开。
5. **断点续传（上传）**：发大文件中途 `$ADB -s emulator-5554 shell am force-stop com.jitong.im` → 重开登录 → 应自动续传完成。
6. **断点续传（下载）**：下载中途杀 App → 重开 → 再点下载从断点续。
7. 选 >100MB 文件 → 提示超限、卡片失败。

- [ ] **Step 4: 最终回归 + Commit（如有手测修复）**

Run: `cd "/Users/xiaozong/zzq/IM Project/IMproject-main/im_server" && ./build/test_e2e`
Expected: `test_e2e PASSED`。

---

## Self-Review 结论

- **Spec 覆盖**：协议(Task1)、服务端上传/下载/校验/清理(Task4/6/7/8)、断点续传(Task5 + water mark)、Room/客户端收发/状态机/UI(Task9~13)、e2e+手测(Task4~7,14) 均有对应任务。
- **类型一致**：`file_id==msg_id`、`FILE_CHUNK_SIZE=256*1024`、`FILE_MAX_SIZE=100MB`、`type=2`、状态码 `FILE_ST_*` 全程一致；`ChatMessage` 文件字段与 `MessageEntity` 同名。
- **已知实现注意点**（非占位符，实现时按注释处理）：
  1. `m_pendingUploads` 跨会话并发访问建议加 `std::mutex`（Task4 Step7 注明）。
  2. e2e 接收方在线 session 变量名（`b3`/`eb3`）以实现时 e2e 现存在线双方为准（Task4 Step9 校正）。
  3. Android `appContext`/`appResolver` 注入需 `MainActivity` 配合（Task11/12 注明）。
