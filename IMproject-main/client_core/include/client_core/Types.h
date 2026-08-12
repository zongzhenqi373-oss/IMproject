#pragma once
// client_core 对外数据类型（纯 C++，与 UI 无关）

#include <cstdint>
#include <string>

namespace im {

// 用户自己信息
struct UserInfo {
    int id = 0;
    int iconId = 0;
    std::string nick;    // UTF-8
    std::string feeling; // UTF-8
};

// 好友信息
struct FriendInfo {
    int id = 0;
    int iconId = 0;
    int status = 1; // 0=在线 1=离线（与 proto::STATUS_* 一致）
    std::string nick;    // UTF-8
    std::string feeling; // UTF-8
};

// 聊天记录（本地存储用）
struct ChatMessage {
    std::int64_t id = 0;   // 本地自增 id
    int peerId = 0;        // 对端用户 id
    bool outgoing = false; // true=我发出的，false=收到的
    std::string content;   // UTF-8
    std::int64_t ts = 0;   // unix 秒
};

} // namespace im
