# 即通 App 技术设计文档

> 配套文档：`jitong_requirements.md`
> 文档状态：`待确认 v2` ｜ 创建：2026-08-17 ｜ 修订：2026-08-17（纳入注册/漫游/文件传输/已读/输入中）

---

## 1. 总体架构

```
┌──────────────────┐        ┌──────────────────┐
│  Android App A    │        │  Android App B    │
│  Compose + Room   │        │  Compose + Room   │
│  + MMKV + Coil    │        │  (Android Studio) │
└────────┬─────────┘        └────────┬─────────┘
         │  TLS / TCP                │
         │  [4B包长][4B类型][pb body] │
         └────────────┬─────────────┘
                      ▼
        ┌─────────────────────────┐
        │  im_server（跨平台 C++）  │
        │  asio Reactor + 线程池   │
        │  protobuf + SQLite       │
        │  VS Code + CMake 构建    │
        └─────────────────────────┘
```

### 1.1 架构选型论证（考察点 1）

| 决策 | 选择 | 备选 | 取舍理由 |
|---|---|---|---|
| 服务端跨平台 | **新建 `im_server`（asio）** | 移植 Windows IMServer | 移植≈重写网络层；顺手完成 roadmap 既定 asio 重写，一次解决"跨平台 + 并发模型" |
| Android 复用 client_core | **否，纯 Kotlin 协议栈** | djinni/JNI | 首版落地优先、降低风险；djinni 复用 C++ 核心作为"后续演进"文档化 |
| 服务端 DB | **SQLite** | MySQL | 部署零依赖；schema 从 `mysql_schema.sql` 平移；连接池解决并发 |
| 本地消息库 | **Room(SQLite)** | 全 MMKV | 消息需关系查询/分页/FTS；MMKV 定位 KV（凭证/配置），双存储对齐 QQNT（MMKV+wcdb） |
| 通道加密 | **TLS（asio::ssl）** | 应用层 TEA+ECDH（QQNT 方案） | TLS 行业标准、路径短；QQNT 方案作面试对比 |

---

## 2. 协议设计（`protocol/im.proto` 扩展）

线格式不变：`[4B 大端包长][4B 小端协议号][pb payload]`。pb 加字段前后兼容。

```proto
enum MsgType {
  TEXT = 0;
  IMAGE = 1;
  FILE = 2;
}

message ChatInfoRq {
  int32 myid = 1;
  int32 friid = 2;
  string msg = 3;            // TEXT
  MsgType type = 4;
  bytes image_data = 5;      // IMAGE（压缩内联）
  int32 image_width = 6;
  int32 image_height = 7;
  string msg_id = 8;         // 客户端 UUID，漫游/去重幂等
}

// ---- 文件传输（分块）----
message FileOfferRq {        // C→S→C：发起文件
  string msg_id = 1;
  int32 myid = 2; int32 friid = 3;
  string file_name = 4;
  int64 file_size = 5;
  string file_hash = 6;      // sha256，秒传/断点校验
  int32 chunk_size = 7;
  int32 total_chunks = 8;
}
message FileChunkRq {        // 逐块发送
  string msg_id = 1;
  int32 seq = 2;
  bytes data = 3;            // 每块 ≤256KB
}
message FileCompleteRq { string msg_id = 1; }  // 发送完毕
message FileProgressRs {     // 回执（进度/失败）
  string msg_id = 1; int32 received_chunks = 2; int32 result = 3;
}

// ---- 消息漫游 ----
message PullHistoryRq {      // C→S：按时间戳分页拉取
  int32 myid = 1; int32 peer_id = 2;
  int64 before_ts = 3;       -- 拉此时间之前
  int32 limit = 4;
}
message HistoryMessage {     -- 一条历史（文本/图片/文件元信息）
  string msg_id = 1; int32 sender_id = 2; int32 receiver_id = 3;
  MsgType type = 4; string content = 5; string media_path = 6; int64 ts = 7;
}
message PullHistoryRs { repeated HistoryMessage list = 1; bool has_more = 2; }

// ---- 已读回执 ----
message ReadReceiptRq { int32 myid = 1; int32 peer_id = 2; repeated string msg_ids = 3; }
// S→发送方 转发 ReadReceiptRq；发送方据此把消息置"已读"

// ---- 输入中（临时态，不落库）----
message TypingRq { int32 myid = 1; int32 friid = 2; bool typing = 3; }
```

**协议号规划**（双端 `Protocol.h`/`def.h` 同步，`DEF_BASE=1000`）：

| 协议号 | 名称 |
|---|---|
| +0 ~ +12 | 现有：注册/登录/资料/聊天/加好友/下线/心跳/互踢 |
| +13/+14/+15/+16 | FILE_OFFER / FILE_CHUNK / FILE_COMPLETE / FILE_PROGRESS |
| +17/+18 | PULL_HISTORY_RQ / PULL_HISTORY_RS |
| +19 | READ_RECEIPT |
| +20 | TYPING |

**文件为什么分块而不内联**：文件可 ≥10MB，单包内联会阻塞信令通道且逼近 `MAX_PACK_LEN`；分块（≤256KB/块）+ msg_id 幂等重组 + 进度回执，重发不重复写（决策记录 D4）。

---

## 3. 后端设计（`im_server/`，重点）

### 3.1 并发模型（核心考察点）

```
main 线程：asio::io_context + acceptor
   ▼
asio::thread_pool io_threads(4)      ← Reactor 多路复用
   │   每连接 Session + strand        ← 同连接读写串行，连接内无锁
   ▼
业务投递 → db_worker 线程池(2)        ← IO/业务两阶段解耦
   ▼
SQLite 连接池（每 worker 一连接）      ← 消除"MySQL 单连接互斥"瓶颈
```
一句话：**Reactor 多路复用 + 每连接 strand 无锁串行 + IO/业务两阶段投递 + DB 连接池**。连接数不再绑定线程数（旧版 thread-per-connection 约 2000 瓶颈）。

### 3.2 模块划分

| 模块 | 职责 |
|---|---|
| `Session` | 单连接：TLS 握手、二段式帧读写 |
| `Dispatcher` | 协议号路由（函数指针表，对齐 client_core） |
| `AuthService` | 注册/登录（加盐哈希）、互踢、token（预留） |
| `MessageService` | 在线转发 / 离线补发 / 图片文件落盘回传 / 漫游分页 / 已读回执转发 / 输入中转发 |
| `PresenceService` | id→session 映射、心跳扫描、好友上下线广播 |
| `DbPool` | SQLite 连接池 + 预处理语句 + WAL 配置 |

> **SQLite 写并发（必须配置）**：SQLite 同一时刻只允许一个写者，多 worker 并发写会直接报 `SQLITE_BUSY`。连接池每个连接打开时执行：
> ```sql
> PRAGMA journal_mode=WAL;   -- 读写不互斥，写串行
> PRAGMA busy_timeout=5000;  -- 写冲突时等待而非立即报错
> ```
> 这正是 QQNT 里 WCDB 做的同类并发优化，演示多客户端并发（UC9）必踩，也是很好的讲解素材。
| 业务平移 | 登录/转发/离线/心跳/互踢 从 `IMServer/Kernel.cpp` 移植 |

### 3.3 服务端 DB Schema（SQLite）

```sql
t_user(id PK, name, tel, passwd VARCHAR(64), salt VARCHAR(32), feeling, iconid)
    UNIQUE(name), UNIQUE(tel)
t_friend(idA, idB, PK(idA,idB))  INDEX idx_idB(idB)
offline_msg(id PK, sender_id, receiver_id, content, is_delivered, send_time)
    INDEX idx_recv(receiver_id, is_delivered, send_time)
-- 全量消息表（漫游 + 图片/文件元信息）
messages(id PK, msg_id UNIQUE, conversation_id INT, sender_id, receiver_id, type, content, media_path, ts, is_read DEFAULT 0)
    INDEX idx_conv_ts(conversation_id, ts)            -- 漫游分页：会话维度最左前缀（对齐 QQNT {PeerUidIndex, MsgTime}）
    INDEX idx_recv_ts(receiver_id, ts)
-- conversation_id 由收发双方 id 生成：min(id)*K + max(id)（K 取远大于用户数上限的常数，如 1<<20）。
-- 双向会话同值（A→B 与 B→A 相同），拉取历史只需 WHERE conversation_id=? ORDER BY ts，
-- 避免 (sender=A AND receiver=B) OR (sender=B AND receiver=A) 导致的单方向全表扫描。
-- 文件分块临时表（重组完成后清理或保留映射）
file_chunks(msg_id, seq, chunk_path, PRIMARY KEY(msg_id, seq))
```

### 3.4 图片/文件服务端流程（考察点 4）

- **图片**：收字节 → 落盘 `uploads/img/<sha256>.jpg` → db 存路径 → 在线转发/离线补发
- **文件**：`FileOffer` → 服务端按 `msg_id` 建临时目录 `uploads/file/<msg_id>/`；`FileChunk` 按 seq 落块并回进度；`FileComplete` 触发校验（hash）+ 合并为最终文件 + 转发/入库；**msg_id 幂等**（重发覆盖同 seq 块，不重复）
- **漫游口径（本期定死）**：文本、图片**直接回传内容**；文件**只回传元信息**（文件名/大小/图标），用户点击后**可选择重新下载**（再走 FileChunk 拉取）。避免演示时点开漫游回来的文件打不开的尴尬

### 3.5 传输安全

- **TLS**：asio::ssl + 自签证书；Android `SSLSocket` + pinning
- **登录协议安全（考察点 2）**：客户端 `SHA-256(密码)` → 服务端 `SHA-256(salt+hash)` 比对；密码不出链路明文、库无原文、随机盐防彩虹表
- **已知权衡：等价口令问题（主动讲成演进点）**。客户端发的是固定 `SHA-256(密码)`，该值本身成为"口令等价物"——若 TLS 被突破（如自签证书 pinning 没配好），攻击者抓到它即可重放登录，无需知道原密码；且 SHA-256 是快哈希，DB 泄露后暴力破解成本低。工业界更完整的做法是 **QQNT 票据制**：密码只用于首次换取 A1/A2 票据，后续鉴权全部走短期票据，密码根本不参与日常通信。本项目的 token 字段正是向该方向的预留（演进路线：票据 + 自动登录）。讲解话术："我知道更优解（票据制）并预留了演进，等价口令风险由 TLS 通道兜底"

---

## 4. Android 前端设计

### 4.1 模块

| 模块 | 技术 | 说明 |
|---|---|---|
| UI | Kotlin + Compose | 注册/登录/好友列表/聊天/图片查看/文件进度/搜索 |
| 网络 | `SSLSocket` + 协程 | 二段式帧；回调→`Dispatchers.Main` |
| 协议 | protobuf-javalite | 与 C++ 共用 `im.proto` |
| 本地库 | Room + FTS4 + SQLCipher | 消息/搜索/加密 |
| KV | **MMKV** | token/当前用户/配置；`cryptKey` |
| 图片/文件 | 相册/文件选择器 + 压缩 + Coil | 图片 ≤1024px q80；文件分块读 |

### 4.2 本地 DB（考察点 3：字段/索引/加密）

```sql
messages(id PK AUTOINCREMENT, msg_id TEXT UNIQUE, conversation_id INT, sender_id INT,
         type INT, content TEXT NULL, media_path TEXT NULL, file_name TEXT NULL,
         ts INT, status INT)     -- status: 0发送中 1已送达 2已读 3失败
  INDEX idx_msg_conv_ts(conversation_id, ts)      -- 聊天页分页（最左前缀）
  INDEX idx_msg_status(conversation_id, status)   -- 失败重发/未读
messages_fts VIRTUAL TABLE USING fts4(content)     -- 独立存储模式（不用 content= 外部内容模式）
conversations(conversation_id PK, peer_nick, last_msg, last_ts, unread_count)
-- FTS 同步策略：插入消息时【同一事务】双写 messages 与 messages_fts。
-- 外部内容模式（content=`messages`）依赖触发器同步，而 Room 对触发器支持别扭，
-- 新手常踩"插了消息但搜不到"的坑；独立存储多存一份文本，但逻辑直白、无同步遗漏。
```

- 索引方法论：查询场景反推、等值在前范围在后、克制数量（写多读多）
- **漫游合并**：拉到的历史按 `msg_id UNIQUE` 幂等插入（`INSERT OR IGNORE`），与本地不重复
- 加密：SQLCipher 整库（key 存 Android Keystore）+ MMKV `cryptKey`；密码不落本地库

### 4.3 关键流程

- **图片**：选图→压缩→发送（内联）→ 本地写路径+Room→送达回执→B 端 Coil 显示
- **文件**：选文件→`FileOffer`→分块读流发送→进度条（本地 + 对方进度回执）→`Complete`→B 端重组落盘可打开
- **漫游**：登录后/进入会话上拉→`PullHistoryRq(before_ts)`→分页→按 `msg_id` 幂等合并→UI 无感刷新
- **已读**：进入会话可见消息→发 `ReadReceiptRq(msg_ids)`→服务端转发→发送方置"已读"
- **输入中**：输入框文本变化→`TypingRq(true)`，5s 无变化→`false`；对端显示"对方正在输入…"并超时消失

---

## 5. 设计决策记录（AI 迭代证据）

| # | 议题 | 方案 A | 方案 B | 结论 | 理由 |
|---|---|---|---|---|---|
| D1 | 服务端跨平台 | 移植 IMServer | **新建 asio im_server** | B | 移植≈重写；顺带升级并发模型 |
| D2 | Android 复用 C++ 核心 | djinni/JNI | **纯 Kotlin 协议栈** | B（本期） | 落地优先；djinni 留作演进并文档化 |
| D3 | 消息存储 | 全 MMKV | **Room(消息)+MMKV(KV)** | B | 关系查询/FTS 需要；双存储对齐 QQNT |
| D4 | 文件传输 | 单包内联 | **分块 + msg_id 幂等** | B | 大文件不阻塞信令通道、可进度、重发安全 |
| D5 | 通道加密 | TEA/ECDH | **TLS** | B | 行业标准路径短；QQNT 方案作对比 |
| D6 | 服务端 DB | MySQL | **SQLite + 连接池** | B | 部署零依赖 |
| D7 | 漫游幂等 | 时间戳去重 | **msg_id UNIQUE + INSERT OR IGNORE** | B | 跨端唯一、幂等可靠 |
| 演进证据 | — | — | — | — | git log：裸 struct→pb、无锁→并发治理、Windows→跨平台 |

---

## 6. 里程碑

| 里程碑 | 内容 | 预估 |
|---|---|---|
| M1 | im.proto 扩展 + `im_server` 骨架（注册/登录+文本+SQLite 跑通） | 1~2 天 |
| M2 | 并发模型（线程池/strand/DB 池）+ 心跳 + 离线补发 | 1~2 天 |
| M3 | 图片消息（内联 + 服务端落盘） | 1 天 |
| M4 | Android：注册/登录 + 好友列表 + 文本聊天 | 2~3 天 |
| M5 | Android：图片收发 + Room + FTS 搜索 + MMKV | 1~2 天 |
| M6 | **消息漫游**（服务端历史 + 分页拉取 + 客户端幂等合并） | 1~2 天 |
| M7 | **文件传输**（分块 + 进度 + 重组） | 1~2 天 |
| M8 | **已读回执 + 输入中** | 1 天 |
| M9 | TLS + 双端联调 + 演示彩排 | 1 天 |

> 裁剪预案（时间紧时）：M8 降级为讲解、M9 TLS 降级为方案讲解+抓包对比，核心 Must（F1-F12）优先保。

---

## 7. 演示方案（本机 macOS 组合）

- 两个 Android 模拟器实例登录 A/B；模拟器经 `10.0.2.2` 访问 Mac 宿主服务端
- 备选第二端：client_core 编的 CLI 客户端 ↔ 模拟器互聊
- VS Code（CMake Tools）一键构建运行 `im_server`，日志展示多连接并发
- 现场展示：`uploads/` 落盘、FTS 搜索、漫游（卸载重装拉回）、文件进度、已读/输入中、互踢

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 范围扩大（漫游/文件/已读/输入中）导致工期紧 | 里程碑 M6-M8 按 Should 可降级；核心 Must 先行 |
| Android 学习曲线 | UI 控制页面数；CLI 客户端兜底演示 |
| 文件分块重组正确性 | msg_id+seq 幂等设计 + 单元/回环测试先行 |
| 漫游合并冲突 | msg_id UNIQUE + INSERT OR IGNORE 从机制上杜绝重复 |
| 模拟器 TLS 自签信任 | networkSecurityConfig；兜底关闭 TLS 仅讲解方案（可接受降级，现场别硬磕） |
| 图片内联占信令通道被追问 | ≤500KB 压缩图内联对 demo 量级无问题；话术："QQNT 富媒体走独立 BDH 通道正是为解决这个问题，我的文件分块+进度回执是轻量版折中" |
| `offline_msg` 与 `messages` 双写一致性 | 约定：在线转发写 `messages`、离线再补写 `offline_msg`；上线补发后标 `is_delivered=1`；编码时配回环测试守这条不变式 |
