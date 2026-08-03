#ifndef KERNAL_H
#define KERNAL_H

#define WIN32_LEAN_AND_MEAN      // 避免包含太多 Windows 头
#define NOMINMAX                 // 避免 min/max 宏污染

// 临时屏蔽 rpcndr.h 的 byte 定义
#ifdef byte
#undef byte
#endif
typedef unsigned char rpc_byte;  // 若确实需要，可自己另定义

#include <QObject>
#include<QTimer>
#include"logindia.h"
#include"mainwdiget.h"
#include"./mediator/INetmediator.h"

class Kernal : public QObject
{
    Q_OBJECT
public:
    explicit Kernal(QObject *parent = nullptr);

    //阶段-1：全链路统一 UTF-8，已删除 utf8ToGb2312/gb2312ToUtf8 互转
    //客户端直接用 QString::toUtf8() 写入 struct，QString::fromUtf8() 读出

signals:
    void signals_responce_ClientData(char*,int ,unsigned long );

public slots:
    void slots_sendRegisterToServe(QString,QString,QString);
    void slots_sendLoginToServe(QString,QString);
    void slots_recvServerData(char*,int,unsigned long);
    void slots_updateFriInfo();   //定时器触发，定时更新朋友信息
    void slots_sendMsgtoServe(QString,int);
    void slots_addFriend(QString);
    void slots_addFriendTimer();   //定时器触发，模拟ccc上线后添加张三
    void slots_FriofflineTimer();   //模拟朋友下线
    void slots_sendMyoffline();   //给服务端发送下线的通知
    void slots_quitLogin();  //退出登录


public:
    LoginDia*m_pLogin;
    mainwdiget* m_Mainwdiget;
    INetmediator* m_pMediator;


    using DEAL_FUN = void(Kernal::*)(char*,int ,unsigned long );
    DEAL_FUN m_dealFunArr[30];

public:
    void deal_registerRs(char*,int,unsigned long);
    void deal_loginRs(char*,int,unsigned long);
    void deal_friendInfo(char*,int,unsigned long);
    void deal_chatInfoRs(char*,int,unsigned long);
    void deal_chatInfoRq(char*,int,unsigned long);
    void deal_addFriRs(char*,int,unsigned long);
    void deal_addFriRq(char*,int,unsigned long);
    void deal_Frioffline(char*,int,unsigned long);

public:
    QTimer*m_pUpateFriTimer;   //定时更新朋友信息
    QTimer*m_pAddFriendTimer;   //模拟ccc上线后，添加张三为好友
    QTimer*m_pFriofflineTimer;  //朋友下线1定时器



};

#endif // KERNAL_H
