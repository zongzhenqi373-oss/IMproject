#pragma once
#ifndef __DEF_H__
#define __DEF_H__

//UDP协议的端口号
#define UDP_PORT (12345)
//TCP协议的端口号
#define TCP_PORT (24563)
//TCP协议队列监听的长度
#define TCP_LISTEN_QUEUE_LEN (100)

#define DEF_BASE   1000

//协议类型的数量（函数指针数组边界，越界校验用）
//注意：客户端与服务端 def.h 必须保持此值一致
#define DEF_PROT_COUNT (30)

//单个包体最大长度（字节），防止异常/恶意超大 RecvLen 导致 OOM/DoS
//10MB，足以容纳当前所有协议 struct（最大 PROT_CHAT_INFO_RQ 约 8KB）
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


//用户昵称长度
#define USER_NICK_LEN   30
//用户电话长度
#define USER_TEL_LEN    15
//用户密码长度
#define USER_PASS_LEN   20
//个性签名长度
#define USER_FEELING_LEN   100

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

//聊天内容长度
#define CHAT_MSG_LEN  (1024*8)

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



using protType = unsigned int;

//协议 struct 必须按 1 字节对齐，保证跨编译器/跨平台 sizeof 一致
//否则不同编译器/位数（x86 vs x64, MSVC vs GCC）下 struct 大小不同，会导致协议错位
#pragma pack(push, 1)

//注册请求
//protocol request
struct PROT_REGISTER_RQ{
    protType prottype;   //协议的类型
    char nick[USER_NICK_LEN];  //昵称
    char tel[USER_TEL_LEN];   //电话
    char pass[USER_PASS_LEN];   //密码

    PROT_REGISTER_RQ():prottype(DEF_PROT_REGISTER_RQ),nick{0},tel{0},pass{0}{}

};

//注册回复
struct PROT_REGISTER_RS{
    protType prottype;   //协议的类型
    int result;
    PROT_REGISTER_RS(int _result = REGISTER_SUCC):prottype(DEF_PROT_REGISTER_RS),result(_result){}

};


//登录请求协议结构
struct PROT_LOGIN_RQ{
    protType prottype;   //协议的类型
    char tel[USER_TEL_LEN];   //电话
    char pass[USER_PASS_LEN];   //密码

    PROT_LOGIN_RQ():prottype(DEF_PROT_LOGIN_RQ),tel{0},pass{0}
    {}
};

//登录回复协议结构
struct PROT_LOGIN_RS{
    protType prottype;   //协议的类型
    int userid;
    int result;

    PROT_LOGIN_RS(int _userid = 0,int _result = LOGIN_SUCCESS):prottype(DEF_PROT_LOGIN_RS),userid(_userid),result(_result)
    {}

};


//朋友信息协议
struct PROT_FRIEND_INFO{
    protType prottype;
    int userid;    //用户id
    int iconid;    //图片头像id
    int status;    //朋友状态，在线或者不在线
    char nick[USER_NICK_LEN];    //昵称
    char feeling[USER_FEELING_LEN];  //个性签名


    PROT_FRIEND_INFO(int _userid = 0,int _iconid = 0,int _status = STATUS_ONLINE )
        :prottype(DEF_PROT_FRIEND_INFO),userid(_userid),iconid(_iconid),status(_status)
    {}
};


//聊天协议的请求
struct PROT_CHAT_INFO_RQ{
    protType prottype;
    int myid;   //自己
    int friid;  //朋友
    char msg[CHAT_MSG_LEN];


    PROT_CHAT_INFO_RQ(int _myid = 0,int _friid = 0):
         prottype(DEF_PROT_CHAT_INFO_RQ),myid(_myid),friid(_friid)
    {}

};


//聊天回复的协议
struct PROT_CHAT_INFO_RS{
    protType prottype;
    int myid;   //朋友
    int friid;   //自己
    int result;  //聊天结果


    PROT_CHAT_INFO_RS(int _myid = 0,int _friid = 0,int _result = CHAT_RESULT_SUCC):
         prottype(DEF_PROT_CHAT_INFO_RS),myid(_myid),friid(_friid),result(_result)
    {}

};

//添加朋友的协议
struct PROT_ADD_FRIEND_RQ{
    protType prottype;
    int myid;   //自己
    char mynick[USER_NICK_LEN];
    char frinick[USER_NICK_LEN];

    PROT_ADD_FRIEND_RQ(int _id = 0):prottype(DEF_PROT_ADD_FRIEND_RQ),myid(_id){}

};

//回复添加朋友的回复
struct PROT_ADD_FRIEND_RS{
    protType prottype;
    int result;
    int destid;
    char destnick[USER_NICK_LEN];
    int myid;
    char mynick[USER_NICK_LEN];

    PROT_ADD_FRIEND_RS(int _result = ADD_FRIEND_AGREE,int _destid = 0,int _myid = 0):prottype(DEF_PROT_ADD_FRIEND_RS),result(_result),destid(_destid),myid(_myid){}

};


//朋友下线请求
struct PROT_FRIEND_OFFLINE{
    protType prottype;
    int offlineid;  //下线朋友的id

    PROT_FRIEND_OFFLINE(int _offlineid = 0):prottype(DEF_PROT_FRIEND_OFFLINE),offlineid(_offlineid){}



};

#pragma pack(pop)



#endif //__DEF_H__
