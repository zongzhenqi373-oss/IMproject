# 文件传输（M7）设计方案

## 目标

在现有文本/图片消息基础上，支持任意类型文件的收发。文件可 ≥10MB，与图片的本质差异在于：图片走内联字节（≤500KB，一个 `ChatInfoRq` 传完），而大文件若内联会堵死唯一那条 TCP 信令通道（登录/聊天/心跳/漫游共用），甚至触发 `MAX_PACK_LEN`（10MB）拒包。因此 M7 采用**独立分片协议**传输，支持断点续传、进度显示、按需下载，大小上限 100MB。

现有铺垫：`im.proto` 的 `MsgType.FILE=2` 已预留、`Server` 已建 `uploads/file/` 目录、"+面板"有"文件"入口（当前仅 toast）。

## 关键决策（brainstorming 已逐节确认）

1. **范围**：分片（≤256KB）+ 进度 + **断点续传**；**不做秒传**（hash 去重）。
2. **接收模型**：消息卡片 + **按需下载**——接收方先收到文件卡片（名/大小/图标），点击才下载；下载也走分片、可断点续传。
3. **传输通道**：复用现有单条 TCP。256KB 分片在块之间可穿插信令（服务端逐包异步处理），最坏只等一个块的时间，无需第二条连接。
4. **大小上限**：100MB（分块 ≤256KB 时约 400 块）。
5. **断点续传机制**：**顺序分片 + 水位线**。客户端顺序发块 0,1,2…；服务端 `.part` 顺序追加，只记"已连续收到 N 块"，`N` 由 `.part` 文件大小推算（无额外元数据，抗重启）。
6. **文件标识**：`file_id = msg_id`（客户端生成的 UUID）。既是续传定位键，也是接收方下载寻址键。
7. **半成品清理**：服务端启动时清理 `uploads/file/tmp/` 下超 24h 未完成的 `.part`。
8. **接收端存储**：app 私有 `filesDir/file/`（免存储权限），FileProvider + `ACTION_VIEW` 交系统应用打开。
9. **文件卡片复用 `ChatInfoRq`**（`type=FILE` + 扩字段），走现有在线转发/离线补发/漫游链路，天然复用去重与排序。

## 协议（协议号 1017~1022，避开 M6 漫游占用的 1013~1016）

`ChatInfoRq` 扩三字段（`type=FILE` 时用）：`file_name`、`file_size`、`file_id`。

```proto
// 上传侧
message FileOfferRq {   // 1017 C→S
  string msg_id = 1;    // 客户端 UUID，全程幂等 / 重组 key / = file_id
  int32  receiver_id = 2;
  string file_name = 3;
  int64  file_size = 4;
  int32  total_chunks = 5;
  string sha256 = 6;    // 完整性校验（非秒传）
}
message FileOfferRs {   // 1018 S→C
  string msg_id = 1;
  string file_id = 2;          // = msg_id
  int32  received_chunks = 3;  // 水位线 N，续传起点
  int32  result = 4;           // OK / 超限拒绝
}
message FileChunkRq {   // 1019 C→S（上传）/ S→C（下载）复用
  string file_id = 1;
  int32  chunk_index = 2;
  bytes  data = 3;             // ≤256KB
}
message FileCompleteRq { // 1020 C→S
  string file_id = 1;
  string msg_id = 2;
}
message FileProgressRs { // 1021 S→C（上传/下载进度共用）
  string file_id = 1;
  int32  received_chunks = 2;
  int32  total_chunks = 3;
  int32  status = 4;          // uploading / verifying / done / failed
}
// 下载侧
message FileDownloadRq { // 1022 C→S
  string file_id = 1;
  int32  from_chunk = 2;      // 续传起点 = 本地已存块数
}
```

协议号同步三处：`client_core/include/client_core/Protocol.h`、`jitong_android/.../net/Protocol.kt`；`im.proto` 改后 `protoc` 重新生成 `protocol/generated/im.pb.{cc,h}`。`DEF_PROT_COUNT=30` 足够（最大 `DEF_BASE+22`）。

## 服务端数据流

**上传**
1. `FileOfferRq`：以 `msg_id` 定位 `uploads/file/tmp/<file_id>.part`，`N = .part 大小 / 256KB`（新文件为 0）；校验 `file_size ≤ 100MB`，超则 `FileOfferRs(result=拒绝)`；否则回 `FileOfferRs(file_id, received_chunks=N)`。用 `s->userId()` 鉴权。
2. `FileChunkRq`：`chunk_index==N` → 追加 `.part`、`N++`；`<N` → 幂等丢弃；`>N` → 忽略（顺序传输不应乱序）。每收若干块回一次 `FileProgressRs(N)`。
3. `FileCompleteRq`：校验 `.part` 大小 == `file_size` 且 `sha256(.part) == Offer 的 sha256` → 移到 `uploads/file/<file_id>_<name>` → `messages` 插一条 `type=FILE` 行（分配 `seq`）→ 文件卡片（`ChatInfoRq type=FILE`）走现有转发/离线补发/漫游链路 → 回发送方 `FileProgressRs(done)`。校验失败删 `.part` 回 `failed`。

**下载**
4. `FileDownloadRq(file_id, from_chunk)`：打开成品，`seek(from_chunk*256KB)`，逐块下发 `FileChunkRq(S→C)`，末尾 `FileProgressRs(done)`；文件缺失回 `failed`。

**存储与清理**：半成品 `uploads/file/tmp/*.part`（水位线由文件大小推算，不进 DB）；成品 `uploads/file/`；`messages` 表加 `file_id`、`file_size` 两列，`content` 存文件名，**不建 file_chunks 表**（块只在 `.part`）。`Server` 启动时清理 `tmp/` 下超 24h 的 `.part`。

**sha256 流式**：`protocol/sha256.h` 现为一次性 `sha256Hex(string)`，100MB 校验需分块读入增量计算（新增流式变体或分块喂入），避免整文件入内存。

## 客户端（Android）数据流

**发送方状态机**：SAF `ACTION_OPEN_DOCUMENT` 选文件 → 流式算 `sha256`、`total_chunks` → 本地插 `type=FILE` 消息（发送中，存 uri/元数据）→ `FileOfferRq` → 收 `FileOfferRs(N)` 从块 N 顺序发 `FileChunkRq` → 按 `FileProgressRs` 刷新进度 → `FileCompleteRq` → `done` 标记已送达。断点续传：Room 记住未完成外发文件消息 + 本地 uri，重连后重发 Offer 从服务端水位线续发。

**接收方状态机**：收到文件卡片（`type=FILE`）→ 插消息（待下载，存 `file_id/名/大小`，本地路径空）→ UI 卡片"下载"按钮 → 点击发 `FileDownloadRq(from_chunk=本地 .part 块数)` → 接收 `FileChunkRq` 追加 `filesDir/file/<file_id>_<name>.part` → `done` 重命名为正式文件、卡片变"打开"（FileProvider）。断点续传：中断后本地 `.part` 保留，再次点下载从 `from_chunk` 续拉。

**Room**：`MessageEntity` 加 `fileId`/`fileName`/`fileSize`/`localPath`(可空)/`transferred`(已传块)；版本升级 + 迁移。
**UI**：文件卡片（图标+名+大小+进度条+动作按钮）；`AndroidManifest.xml` 加 `FileProvider` + `res/xml/file_paths.xml`。

## 错误处理与边界

- **中断/重连/杀进程**：上传靠服务端水位线（重发 Offer 续发），下载靠本地 `.part`（`from_chunk` 续拉）。
- **sha256 校验失败**：服务端删 `.part` 回 `failed`，发送方卡片置失败可重试（`.part` 已删则 N=0 重传）。
- **超 100MB**：`FileOfferRs(result=拒绝)`，发送方提示。
- **乱序/重复块**：服务端幂等（`<N` 丢、`>N` 忽略、只认 `==N`）。
- **下载时服务端文件缺失**：`FileProgressRs(failed)`，接收方提示"文件已失效"。
- **接收方离线**：卡片经离线补发/漫游补达；文件本体留服务端等下载（`tmp/` 清理只针对半成品，成品不清）。
- **信令不被堵**：256KB 分片在块间穿插心跳/聊天；多文件并发各按 `file_id` 独立，服务端每 `.part` 顺序写。

## 测试

- **服务端 e2e**（主保护，仿 M6）：扩 `ClientCore`/`IClientEvents` 加文件收发方法与回调（默认空实现，不破坏现有实现）；`test_e2e` 覆盖多块上传→水位线正确→Complete 校验+落盘、中断续传（重发 Offer 返回 N>0 续发）、下载从中途 `from_chunk` 续拉、超限拒绝、sha256 不符置 `failed`、文件卡片经离线补发送达。
- **Android**：编译（`assembleDebug`）+ 手测（发文件、杀 App 中途续传、接收下载中断续传、FileProvider 打开、超限被拒、离线接收方上线后收卡片再下载）。

## 实施顺序

1. `im.proto` 加 6 个 message + 扩 `ChatInfoRq`，重新生成各端代码；协议号常量同步三处。
2. `protocol/sha256.h` 加流式 sha256。
3. 服务端：`Database` 加列 + 文件消息落库；`Dispatcher` 加 4 个 handler + 启动清理；`Server` 建 `tmp/` 目录。
4. C++ 客户端 `ClientCore`/`IClientEvents` 加文件收发；`test_e2e` 加 M7 断言。
5. Android：`ImClient` 收发方法 + Event；`MainViewModel` 两套状态机 + 断点续传；`ChatScreen` SAF + 文件卡片；Room 迁移；`FileProvider`。
6. 编译 + e2e 验证（服务端）、手测（客户端）。
