# 消息漫游（M6）设计方案

## 目标
换设备 / 重装 App 后，客户端本地 Room 库被清空，但服务端 `messages` 表历史仍在。
通过“消息漫游”让客户端登录后主动从服务端拉回历史消息，恢复聊天记录。

## 关键决策（已确认）
1. **范围**：分页——每会话先拉最近 N 条，聊天页上拉加载更早。
2. **时机（混合）**：登录后只拉每会话最后 1 条（会话列表预览）；点开会话才拉最近 N 条 + 上拉翻页。
3. **分页游标**：用会话级 `seq`（请求“某会话、比 seq X 更早的 N 条”）。
4. **图片**：漫游时读盘回传完整图片字节（复用离线补发逻辑）。
5. **协议**：独立下行协议（对齐 QQNT——通道独立、QoS 隔离、避免队头阻塞），但**消息体格式与聊天复用**（同一张消息表、同一套 msgId 唯一约束防重）。
6. **hasMore 判定**：简单版（返回满 N 条即认为还有更多，最后可能多一次空拉，无害）。
7. **上拉滚动体验**：简单版（允许列表轻微跳动，不做滚动锚点）。

## 协议新增（现有协议号到 DEF_BASE+12）
```
DEF_PROT_ROAM_CONV_RQ = DEF_BASE + 13  // 会话列表漫游请求（登录后，每会话最后1条）
DEF_PROT_ROAM_CONV_RS = DEF_BASE + 14  // 会话列表漫游响应
DEF_PROT_ROAM_MSG_RQ  = DEF_BASE + 15  // 会话历史分页请求
DEF_PROT_ROAM_MSG_RS  = DEF_BASE + 16  // 会话历史分页响应
```

### PB 结构（im.proto）
```proto
message RoamConvRq { int32 myid = 1; }
message RoamConvRs {
  repeated ChatInfoRq convs = 1;   // 每会话最后一条，复用 ChatInfoRq 消息体
}
message RoamMsgRq {
  int32 myid = 1;
  int32 peerId = 2;
  int64 beforeSeq = 3;             // 游标：首次传极大值(拉最新)，上拉传当前已加载最小 seq
  int32 limit = 4;                 // 本批数量 N
}
message RoamMsgRs {
  int32 peerId = 1;
  repeated ChatInfoRq msgs = 2;    // 本批历史（服务端 seq 倒序；客户端落地后按 seq 升序）
  bool hasMore = 3;                // 是否还有更早的
  int64 minSeq = 4;                // 本批最小 seq（下次上拉游标）
}
```
> 消息体复用 `ChatInfoRq`（含 myid/friid/type/msg/图片字节/msg_id/ts/seq），
> 客户端解码复用、落库复用同一张 Room 表、同一套 msgId 去重。

## 服务端 DB（Database.h/.cpp）
```cpp
// 每会话最后一条（会话列表预览）
std::vector<StoredMessage> roamConversations(int userId);
// 某会话比 beforeSeq 更早的 limit 条（seq 倒序）
std::vector<StoredMessage> roamMessages(int userId, int peerId, std::int64_t beforeSeq, int limit);
```
SQL：
```sql
-- roamConversations：该用户每个会话取 id 最大的一条
SELECT * FROM messages WHERE id IN (
  SELECT MAX(id) FROM messages
  WHERE sender_id=? OR receiver_id=? GROUP BY conversation_id
) ORDER BY ts DESC;

-- roamMessages：某会话比游标更早的 N 条
SELECT * FROM messages
WHERE conversation_id=? AND seq < ?
ORDER BY seq DESC LIMIT ?;
```
- conversation_id = makeConversationId(userId, peerId)
- 图片消息读盘回传字节（复用离线补发那段）
- hasMore = (本批返回条数 == limit)

## 服务端 Dispatcher
- `onRoamConvRq`：查 roamConversations → 组 RoamConvRs（每条填 ChatInfoRq，含 seq/ts/图片字节）→ deliver ROAM_CONV_RS。
- `onRoamMsgRq`：查 roamMessages(peerId, beforeSeq, limit) → 组 RoamMsgRs（msgs + hasMore + minSeq）→ deliver ROAM_MSG_RS。
- 会话对端 id：sender/receiver 里非 userId 的那个。

## 客户端（Android）
### 触发
- 登录成功（LOGIN_SUCCESS，hydrate 之后）：发 ROAM_CONV_RQ。
- 点开会话（openChat）：发 ROAM_MSG_RQ(peerId, beforeSeq=Long.MAX, limit=N)。
- 上拉加载（ChatScreen 滚到顶）：发 ROAM_MSG_RQ(peerId, beforeSeq=该会话已加载最小 seq, limit=N)。

### 落地（复用现有链路）
- ROAM_CONV_RS：每条最后消息 → 更新 _conversations（列表预览）+ append 入库。
- ROAM_MSG_RS：每条 → append（msgId 幂等去重）+ store.save；sortBySeq 保证有序；
  记录该会话 minSeq/hasMore 供上拉。

### 分页状态
- 每会话维护 loadedMinSeq、hasMore。
- MainViewModel 新增 loadMoreHistory(peerId)。
- hasMore=false 停止上拉。

### 去重
- 漫游消息与实时/离线消息重叠时靠 msgId 去重（内存 append 检查 + Room UNIQUE），天然不重复。

## 实施顺序
1. im.proto 加 4 个 message + 协议号常量，重新生成各端代码。
2. 服务端 Database 加两个查询方法。
3. 服务端 Dispatcher 加两个处理函数。
4. Android：ImClient 加请求发送 + 响应解析事件；MainViewModel 加触发与落地与分页状态；ChatScreen 上拉加载。
5. 编译 + e2e 验证（服务端）、重装验证（客户端）。
