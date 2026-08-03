# 阶段 -1 待 Windows 验证清单

> 本机 macOS 无 Qt / 无 WinSock 编译环境，以下改动均在 macOS 上完成代码编辑，**需在 Windows 电脑编译验证**。
> 生成时间：2026-08-03

---

## 1. 改动文件清单

### 客户端（IMClient，Qt 工程）

| 文件 | 改动内容 |
|---|---|
| `net/def.h` | 新增 `MAX_PACK_LEN`、`DEF_PROT_COUNT` 宏；struct 区域加 `#pragma pack(push,1)/pop` |
| `net/TCPClient.cpp` | `sendData` 加 `htonl` + `MAX_PACK_LEN` 校验；`recvData` 加 `ntohl` + 长度上限校验 + 异常路径内存释放 |
| `kernal.h` | 删除 `utf8ToGb2312` / `gb2312ToUtf8` 声明 |
| `kernal.cpp` | 删除两个转码函数实现；发送端 3 处改 `toUtf8()`+`strncpy_s`；接收端 4 处改 `QString::fromUtf8`；`slots_recvServerData` 加越界校验；清理无用 include |

### 服务端（IMServer，VS 工程）

| 文件 | 改动内容 |
|---|---|
| `net/def.h` | 同客户端（已 diff 确认两端完全一致） |
| `net/TCPServer.h` | 加 `#include<mutex>`；新增 `m_addrFromMutex` 成员 |
| `net/TCPServer.cpp` | `sendData` 加 `htonl` + `MAX_PACK_LEN`；`recvData` 加 `ntohl` + 长度校验 + 异常内存释放；`acceptThread`/`recvData`/`unInitNet` 对 `m_addrFrom` 加锁；`recvData` 用 `find` 替代 `count+[]` 消除 TOCTOU |
| `MySQL/CMySql.h` | 新增 `EscapeString` 方法声明 |
| `MySQL/CMySql.cpp` | `mysql_set_character_set` 改 `utf8mb4`；实现 `EscapeString`（包装 `mysql_real_escape_string`） |
| `Kernel.h` | 加 `#include<mutex>`；新增 `m_mapIdtoSocketMutex` + 4 个加锁辅助方法声明（`getSocket`/`setSocket`/`isOnline`/`eraseSocket`） |
| `Kernel.cpp` | 所有 `m_mapIdtoSocket` 访问改走辅助方法（锁内查询，锁外 `sendData`）；`DealRegisterRq`/`DealLoginRq`/`DealChatRq`/`DealAddFriendRq` 的用户输入字符串经 `EscapeString` 转义防 SQL 注入；`DealChatRq` 离线消息 SQL 缓冲区扩容（修潜在栈溢出） |

---

## 2. 编译前必做：MySQL 字符集迁移

> **关键**：服务端 `CMySql` 连接字符集已从 `gb2312` 改为 `utf8mb4`，但 MySQL **表/列字符集**仍是旧的。如果不改，中文会出现乱码或插入失败。

在 MySQL 里执行（用 root 连接 `20250113im` 数据库）：

```sql
-- 1. 修改数据库默认字符集
ALTER DATABASE `20250113im` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 2. 修改 t_user 表及列
ALTER TABLE t_user CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 3. 修改 t_friend 表
ALTER TABLE t_friend CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 4. 修改 offline_msg 表
ALTER TABLE offline_msg CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

-- 5. 验证（应全部显示 utf8mb4）
SELECT TABLE_NAME, TABLE_COLLATION FROM information_schema.TABLES
WHERE TABLE_SCHEMA = '20250113im';
```

> ⚠️ 如果已有 GB2312 编码的旧数据，`CONVERT TO` 可能导致旧中文乱码。建议先备份数据库：
> ```bash
> mysqldump -u root -p 20250113im > backup_before_utf8mb4.sql
> ```
> 如果旧数据不重要（学习项目），可以直接 `TRUNCATE` 清空再迁移。

---

## 3. 编译步骤

### 服务端（VS 工程）
1. 用 VS 打开 `IMServer.vcxproj`（Toolset v142/v145）。
2. 确认 MySQL 开发库 `libmysql.lib` / `mysql.h` 路径配置不变。
3. 重新生成（Rebuild）。
4. **预期编译错误**：无。若报错常见原因：
   - `strnlen_s` 未声明 → 确认编译器支持 C++11 `<string.h>`（MSVS 2015+ 自带）。
   - `mutex` 未找到 → 确认 `<mutex>` 已 include（Kernel.h / TCPServer.h 已加）。
   - `lock_guard` 未找到 → 同上。
5. 运行 `IMServer.exe`，应看到 `TCPServer::WSAStartup success!` / `bind success!`。

### 客户端（Qt 工程）
1. 用 Qt Creator 打开 `IMClient.pro`。
2. 重新构建（Rebuild All）。
3. **预期编译错误**：无。若报错常见原因：
   - `strncpy_s` / `_TRUNCATE` 未声明 → MSVC 自带，Qt MSVC 工具链应支持；若用 MinGW 工具链可能不识别 `strncpy_s`，需改用 `strncpy` + 手动置末尾 `\0`（见下方"兼容性备注"）。
   - `QString::fromUtf8` / `toUtf8` → Qt5+ 自带，无需额外配置。
4. 运行客户端，应能弹出登录窗口。

### 兼容性备注（MinGW 工具链）
如果客户端用 MinGW 而非 MSVC 编译，`strncpy_s` + `_TRUNCATE` 可能不可用。替换方案：
```cpp
// 替换所有 strncpy_s(dst, DST_LEN, src, _TRUNCATE) 为：
{
    QByteArray ba = str.toUtf8();
    int n = min((int)ba.size(), DST_LEN - 1);
    memcpy(dst, ba.constData(), n);
    dst[n] = '\0';
}
```
但原代码大量使用 `strcpy_s`（MSVC 扩展），推测工程默认用 MSVC 工具链，应该没问题。

---

## 4. 回归测试用例

> 阶段 -1 原则：**功能不回归**。以下用例必须在改动后全部通过。

| # | 用例 | 预期 | 涉及改动 |
|---|---|---|---|
| 1 | 注册新用户（昵称含中文，如"张三"） | 注册成功，数据库 `t_user.name` 存 UTF-8 字节 | UTF-8 编码、EscapeString、utf8mb4 |
| 2 | 注册重复昵称 | 提示"昵称已存在" | EscapeString |
| 3 | 注册重复电话 | 提示"电话号码已存在" | EscapeString |
| 4 | **SQL 注入测试**：注册昵称填 `a';DROP TABLE t_user;--` | 注册失败但不崩库，`t_user` 表未被删除 | EscapeString |
| 5 | 登录已注册用户 | 登录成功，主窗口弹出，显示自己的昵称/签名（中文正常） | UTF-8、加锁 |
| 6 | 登录密码错误 | 提示"密码错误" | EscapeString |
| 7 | 两个用户互为好友，A 给 B 发消息（中文） | B 收到中文消息，不乱码 | UTF-8 全链路 |
| 8 | 给离线好友发消息 | 提示"对方已离线"，`offline_msg` 表新增记录，content 为 UTF-8 | EscapeString、SQL 缓冲区扩容 |
| 9 | 离线好友登录后收到离线消息 | 离线消息正确投递，中文不乱码 | UTF-8、加锁 |
| 10 | 添加好友（输入对方昵称，中文） | 对方收到请求弹窗，同意后双向成为好友 | UTF-8、EscapeString |
| 11 | 用户下线 | 好友列表中该用户状态变离线 | 加锁 |
| 12 | **并发测试**：3+ 个客户端同时登录不同用户互发消息 | 无崩溃、无消息错乱、无 socket 串台 | 加锁（m_mapIdtoSocket / m_addrFrom） |
| 13 | **恶意包测试**：用 nc/telnet 连接 24563 端口，发送 4 字节 `0xFFFFFFFF` 作为包长 | 服务端打印 `illegal RecvLen` 并关闭连接，**不崩溃不 OOM** | MAX_PACK_LEN |

> 用例 4 和 13 是阶段 -1 重点验证项——分别验证 SQL 注入修复和长度保护。

---

## 5. 风险点

| 风险 | 等级 | 说明 | 缓解 |
|---|---|---|---|
| `#pragma pack` 改变 struct sizeof | 中 | 加 `pack(1)` 后 struct 不再有填充字节，`sizeof(PROT_XXX)` 可能变小。客户端/服务端**必须同时重新编译**，否则协议错位 | 确保两端同时用新代码，不要混用新旧二进制 |
| UTF-8 中文比 GB2312 长 | 低 | 同样中文 UTF-8 占 3 字节、GB2312 占 2 字节。`nick[30]` 原可放 15 个中文字，现在 10 个 | 用 `strncpy_s+_TRUNCATE` 已防溢出，超长截断 |
| 加锁死锁 | 低 | `m_mapIdtoSocket` 锁内只做 map 操作，`sendData` 在锁外，理论上不会死锁 | 用例 12 并发测试验证 |
| MySQL 旧数据迁移乱码 | 中 | 已有 GB2312 数据转 utf8mb4 可能乱码 | 学习项目建议清空数据重来；正式数据先备份 |
| `strnlen_s` 在老 MSVC 不可用 | 低 | VS2015+ 支持 | 推测工程用 v142/v145（VS2019+），无问题 |

---

## 6. 回退指引

如果改动后编译/运行有问题，可按文件回退（git 未启用，需手动）：

| 文件 | 回退方式 |
|---|---|
| 客户端 def.h | 删除 `MAX_PACK_LEN` / `DEF_PROT_COUNT` 宏、`#pragma pack` 三段 |
| 客户端 TCPClient.cpp | 还原 sendData/recvData 为原版（去 htonl/ntohl/长度校验） |
| 客户端 kernal.cpp | 还原转码函数 + 调用点 + 越界检查（越界检查可保留，是纯增强） |
| 服务端 TCPServer.cpp | 还原 sendData/recvData + 去锁 |
| 服务端 CMySql.cpp | `utf8mb4` 改回 `gb2312`；删除 `EscapeString` 实现 |
| 服务端 Kernel.cpp | 还原 `m_mapIdtoSocket` 直接访问 + 去 `EscapeString` 调用 |

> 建议优先尝试**单点回退**（如只回退加锁，保留协议修复），定位问题范围。

---

## 7. 阶段 -1 完成标准

全部满足后可进入阶段 0：

- [ ] 客户端 + 服务端在 Windows 编译通过
- [ ] MySQL 表字符集已迁移 utf8mb4
- [ ] 13 个回归用例全部通过
- [ ] SQL 注入测试（用例 4）通过
- [ ] 恶意包测试（用例 13）通过
- [ ] 并发测试（用例 12）通过
- [ ] 中文消息全链路无乱码

---

## 8. 决策说明：线程池推迟

原规划阶段 -1 包含"补线程池（方案 A）"。本次**未实现 worker 线程池**，原因：

1. **无法编译验证**：worker 池是大型并发架构改造，在 macOS 无法验证，死锁/竞争 bug 难排查。
2. **与阶段 0 重叠**：方案 A 的任务队列 + worker 池，与阶段 0「服务端 asio 重写」高度重叠——届时 net 层重写会引入更干净的 reactor 线程模型，做两次反而浪费。
3. **加锁已解决 P0**：数据竞争是安全项（已修复），线程池是性能优化（非安全），当前 thread-per-connection + 加锁能稳定支撑数百连接（学习项目够用）。

本次完成了：
- ✅ `m_mapIdtoSocket` 加锁（4 个辅助方法）
- ✅ `m_addrFrom` 加锁

推迟到阶段 0：
- ⏸ worker 线程池 + 任务队列
- ⏸ 真正的 `mysql_stmt_*` 预处理（本次用 `mysql_real_escape_string` 替代）

规划文档已同步更新此决策。
