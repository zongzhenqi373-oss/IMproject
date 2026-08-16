#pragma once
#ifndef __DEF_H__
#define __DEF_H__

// IM 协议常量与编码工具（服务端）
// 阶段 P2：包体已迁移 protobuf（定义见 ../protocol/im.proto，生成物在 ../protocol/generated/）
// 本文件只保留：协议号常量 / 业务结果码 / 字段软上限 / 编码工具
// 线格式：[4B 大端包长][4B 小端协议号][pb payload]
// 注意：协议号与结果码必须与客户端 client_core/Protocol.h 保持一致。

//UDP协议的端口号
#define UDP_PORT (12345)
//TCP协议的端口号
#define TCP_PORT (24563)
//TCP协议队列监听的长度
#define TCP_LISTEN_QUEUE_LEN (100)

#define DEF_BASE   1000

//协议类型的数量（函数指针数组边界，越界校验用）
//注意：客户端与服务端必须保持此值一致
#define DEF_PROT_COUNT (30)

//单个包体最大长度（字节），防止异常/恶意超大 RecvLen 导致 OOM/DoS
//10MB，含 4 字节协议号 + pb payload
#define MAX_PACK_LEN (10 * 1024 * 1024)

//注册请求协议类型
#define DEF_PROT_REGISTER_RQ   (DEF_BASE+0)
//注册回复协议类型
#define DEF_PROT_REGISTER_RS   (DEF_BASE+1)
//登录请求协议类型
#define DEF_PROT_LOGIN_RQ   (DEF_BASE+2)
//登录回复协议类型
#define DEF_PROT_LOGIN_RS   (DEF_BASE+3)
//储存信息协议类型
#define DEF_PROT_FRIEND_INFO   (DEF_BASE+4)
//聊天请求协议类型
#define DEF_PROT_CHAT_INFO_RQ   (DEF_BASE+5)
//聊天回复协议类型
#define DEF_PROT_CHAT_INFO_RS   (DEF_BASE+6)
//添加好友协议类型
#define DEF_PROT_ADD_FRIEND_RQ  (DEF_BASE+7)
//添加好友回复协议类型
#define DEF_PROT_ADD_FRIEND_RS  (DEF_BASE+8)
//下线请求协议类型
#define DEF_PROT_FRIEND_OFFLINE (DEF_BASE+9)
//心跳请求协议类型（客户端→服务端，空 payload）
#define DEF_PROT_HEARTBEAT_RQ  (DEF_BASE+10)
//心跳回复协议类型（服务端→客户端，空 payload）
#define DEF_PROT_HEARTBEAT_RS  (DEF_BASE+11)
//被踢下线通知（服务端→客户端，空 payload：同账号在别处登录）
#define DEF_PROT_KICKED_OFFLINE (DEF_BASE+12)

//心跳超时扫描间隔（秒）
#define HEARTBEAT_SCAN_INTERVAL_SEC (10)
//心跳超时时间（秒）：超过该时间未收到某连接的任何数据则强制下线
#define HEARTBEAT_TIMEOUT_SEC (90)

//字段软上限（字节，UTF-8）—— pb 字符串不再定长，由应用层截断保护
//用户昵称长度
#define USER_NICK_LEN   30
//用户电话长度
#define USER_TEL_LEN    15
//用户密码长度
#define USER_PASS_LEN   20
//个性签名长度
#define USER_FEELING_LEN   100
//聊天内容长度
#define CHAT_MSG_LEN  (1024*8)

//注册成功
#define REGISTER_SUCC  1
//注册失败,昵称已存在
#define REGISTER_NICK_EXIT  2
//注册失败，电话号码已存在
#define REGISTER_TEL_EXIT  3

//登录成功
#define LOGIN_SUCCESS   0
//登录失败——用户名不存在
#define LOGIN_NOTEXIT   1
//登录失败——密码不正确
#define LOGIN_PASSERROR 2

//朋友在线状态
#define STATUS_ONLINE  0
//朋友离线状态
#define STATUS_OFFLINE  1

//聊天成功
#define CHAT_RESULT_SUCC  0
//聊天失败
#define CHAT_RESULT_FAIL  1

//同意添加好友
#define ADD_FRIEND_AGREE 0
//拒绝添加好友
#define ADD_FRIEND_REJECT 1
//对方离线
#define ADD_FRIEND_OFFLINE 2
//用户不存在
#define ADD_FRIEND_NOTEXIT 3

using protType = unsigned int; // 线上为小端 4 字节（x86/x64 主机序即小端，直接读写即可）
static_assert(sizeof(protType) == 4, "protType must be 4 bytes");

// UTF-8 安全截断：不超过 maxBytes 且不切断多字节字符（防御客户端发超长字段）
#include <string>
inline std::string utf8Truncate(const std::string& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    // 回退到字符边界（continuation byte 形如 10xxxxxx）
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

#endif //__DEF_H__
