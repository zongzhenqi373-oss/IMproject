#include "kernal.h"
#include<QDebug>
#include <QByteArray>
#include <QString>
#include"./net/def.h"
#include<QMessageBox>
#include"mainwdiget.h"
#include"./mediator/TCPClientmediator.h"

#define DATA_RESCLIENT(protname) {\
                                 int len_RS = sizeof(protname);\
                                 char *pbuf = new char[len_RS]();\
                                 memcpy(pbuf,&protname,len_RS);\
                                 emit signals_responce_ClientData(pbuf,len_RS,0);\
                                 }


Kernal::Kernal(QObject *parent) : QObject(parent)
    ,m_pLogin(new LoginDia),m_Mainwdiget(new mainwdiget),m_pMediator(new TCPClientmediator)
{


    //打开网络
    if(!m_pMediator->openNet())
    {
        QMessageBox::about(m_pLogin,"提示！","打开网络失败！");
        exit(-1);
    }

    //发送数据信号和槽函数绑定链接
    connect(m_pMediator,SIGNAL(signals_recvServerData(char*,int,unsigned long)),this,SLOT(slots_recvServerData(char*,int,unsigned long)));

    //登录与信号绑定链接
    connect(m_pLogin,SIGNAL(signals_sendRegisterInfo(QString,QString,QString)),this,SLOT(slots_sendRegisterToServe(QString,QString,QString)));
    connect(m_pLogin,SIGNAL(signals_sendLoginInfo(QString,QString)),this,SLOT(slots_sendLoginToServe(QString,QString)));

    //服务端向客户端回复的信号与服务端接收信号进行绑定链接
    connect(this,SIGNAL(signals_responce_ClientData(char*,int ,unsigned long )),this,SLOT(slots_recvServerData(char*,int ,unsigned long )));

    //绑定mainwdiget发送给kernal的信号和kernal发送给服务端的槽函数
    connect(m_Mainwdiget,SIGNAL(signals_sendMsgAndIdtoKernal(QString,int)),this,SLOT(slots_sendMsgtoServe(QString,int)));

    //绑定添加朋友信号和槽函数的连接
    connect(m_Mainwdiget,SIGNAL(signals_addFriend(QString)),this,SLOT(slots_addFriend(QString)));

    //
    connect(m_Mainwdiget,SIGNAL(signals_sendMyoffline()),this,SLOT(slots_sendMyoffline()));

    //
    connect(m_pLogin,SIGNAL(signals_quitLogin()),this,SLOT(slots_quitLogin()));

    //回复处理函数数组，通过协议类型作为下标访问相应的回复处理函数
    m_dealFunArr[DEF_PROT_REGISTER_RS-DEF_BASE] = &Kernal::deal_registerRs;
    m_dealFunArr[DEF_PROT_LOGIN_RS-DEF_BASE] = &Kernal::deal_loginRs;
    m_dealFunArr[DEF_PROT_FRIEND_INFO-DEF_BASE] = &Kernal::deal_friendInfo;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RS-DEF_BASE] = &Kernal::deal_chatInfoRs;
    m_dealFunArr[DEF_PROT_CHAT_INFO_RQ-DEF_BASE] = &Kernal::deal_chatInfoRq;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RS-DEF_BASE] = &Kernal::deal_addFriRs;
    m_dealFunArr[DEF_PROT_ADD_FRIEND_RQ-DEF_BASE] = &Kernal::deal_addFriRq;
    m_dealFunArr[DEF_PROT_FRIEND_OFFLINE-DEF_BASE] = &Kernal::deal_Frioffline;


    m_pLogin->show();

    //-----------------定时器-----------------------------
    m_pUpateFriTimer = new QTimer;
    m_pUpateFriTimer->setSingleShot(true);  //设置定时器单次生效
    connect(m_pUpateFriTimer,SIGNAL(timeout()),this,SLOT(slots_updateFriInfo()));

    m_pAddFriendTimer = new QTimer;
    m_pAddFriendTimer->setSingleShot(true);
    connect(m_pAddFriendTimer,SIGNAL(timeout()),this,SLOT(slots_addFriendTimer()));

    m_pFriofflineTimer = new QTimer;
    m_pFriofflineTimer->setSingleShot(true);
    connect(m_pFriofflineTimer,SIGNAL(timeout()),this,SLOT(slots_FriofflineTimer()));


}

//阶段-1：全链路统一 UTF-8，已删除 utf8ToGb2312/gb2312ToUtf8 实现
//原 GB2312 互转已移除，客户端 struct 内 char[] 直接存 UTF-8 字节，
//服务端 MySQL 连接字符集已改为 utf8mb4，表字符集需同步改（见验证清单）。


void Kernal::slots_recvServerData(char*pbuf,int len,unsigned long from)
{
    qDebug()<<__func__;
    /*--------*/qDebug()<<"服务端处理中...";/*----------*/
    protType type = *(protType*)pbuf;
    //范围校验（防止非法/越界 type 导致 m_dealFunArr 数组越界崩溃）
    int index = type - DEF_BASE;
    if (index >= 0 && index < DEF_PROT_COUNT && type >= DEF_BASE)
    {
        qDebug()<<"合法类型："<<type;

        //二次校验：函数指针必须已注册（未注册的槽位为 nullptr）
        DEAL_FUN pFun = m_dealFunArr[index];
        if (pFun)
        {
            (this->*pFun)(pbuf,len,from);
        }
        else
        {
            qDebug()<<"未注册的协议类型，type:"<<type;
        }
    }
    else
    {
        qDebug()<<"类型不合法，无法跳转回复！type:"<<type;
    }

    if(pbuf)
    {
        delete[]pbuf;
        pbuf = nullptr;
    }
}


//发送注册信息到服务端
void Kernal::slots_sendRegisterToServe(QString nick,QString tel,QString pass)
{
    //arg 分别替换字符串中的  %数字， 按照数字由大到小依次进行替换
    qDebug()<<QString("nick:%1, tel:%2, pass:%3").arg(nick).arg(tel).arg(pass);

    PROT_REGISTER_RQ  registerRq;
    //toStdString:转化为标准库的字符串，QString->std::string
    //c_str：标准库字符串转化为原始字符串，std::string->const char*
    //strcpy_s(registerRq.nick,USER_NICK_LEN,nick.toStdString().c_str() );
    //全链路 UTF-8：QString 直接转 UTF-8 字节写入 struct（strncpy_s + _TRUNCATE 安全截断）
    {
        QByteArray ba = nick.toUtf8();
        strncpy_s(registerRq.nick, USER_NICK_LEN, ba.constData(), _TRUNCATE);
    }
    strcpy_s(registerRq.tel,USER_TEL_LEN,tel.toStdString().c_str() );
    strcpy_s(registerRq.pass,USER_PASS_LEN,pass.toStdString().c_str() );

    /*-------*/qDebug()<<"服务端接收中...";/*----------*/

    //发送给服务端
    qDebug()<<QString("服务端接收到了注册请求：nick:%1, tel:%2, pass:%3").arg(registerRq.nick).arg(registerRq.tel).arg(registerRq.pass);

    m_pMediator->sendData((char*)&registerRq,sizeof(registerRq),23);

}

//发送登录信息到服务端
void Kernal::slots_sendLoginToServe(QString tel,QString pass)
{
    qDebug()<<QString("tel:%2, pass:%3").arg(tel).arg(pass);

    //定义登录协议结构
    PROT_LOGIN_RQ loginRq;
    strcpy_s(loginRq.tel,USER_TEL_LEN,tel.toStdString().c_str() );
    strcpy_s(loginRq.pass,USER_PASS_LEN,pass.toStdString().c_str() );

    /*-------*/qDebug()<<"服务端接收中...";/*----------*/

    //将登录协议发送给服务端
    qDebug()<<QString("服务端接收到了登录请求：tel:%2, pass:%3").arg(loginRq.tel).arg(loginRq.pass);
    m_pMediator->sendData((char*)&loginRq,sizeof(loginRq),23);

}

//更新已有朋友的信息
void Kernal::slots_updateFriInfo()
{
    //模拟服务端，把隔壁老王的新的信息发送给客户端
    PROT_FRIEND_INFO frie3_info(12,21,STATUS_ONLINE);
    strcpy_s(frie3_info.nick,USER_NICK_LEN,"老王头");
    strcpy_s(frie3_info.feeling,USER_FEELING_LEN,"我是一名音乐热爱者");
    DATA_RESCLIENT(frie3_info)
}

//发送信息给服务端
void Kernal::slots_sendMsgtoServe(QString msg,int friid)
{
    //qDebug()<<"msg:"<<msg<<"id:"<<friid;

    //定义聊天协议的请求，填入信息发送给服务端
    PROT_CHAT_INFO_RQ chatInfoRq(m_Mainwdiget->getid(),friid);
    //注意：上面这行先用 toStdString 填了 msg，下面用 UTF-8 覆盖
    //全链路 UTF-8：msg 直接转 UTF-8 字节写入 struct（strncpy_s + _TRUNCATE 安全截断）
    {
        QByteArray ba = msg.toUtf8();
        strncpy_s(chatInfoRq.msg, CHAT_MSG_LEN, ba.constData(), _TRUNCATE);
    }
    //发送给服务端，服务端接收
    qDebug()<<QString("服务端收到了%1发送给%2的聊天请求").arg(chatInfoRq.myid).arg(chatInfoRq.friid);

    m_pMediator->sendData((char*)&chatInfoRq,sizeof(chatInfoRq),12);
}

//添加朋友
void Kernal::slots_addFriend(QString nick)
{
    qDebug()<<"slots_addFriend"<<nick;

    PROT_ADD_FRIEND_RQ addFriRq(m_Mainwdiget->getid());
    strcpy_s(addFriRq.mynick,USER_NICK_LEN,m_Mainwdiget->getmyNick().toStdString().c_str());
    //strcpy_s(addFriRq.frinick,USER_NICK_LEN,nick.toStdString().c_str());

    //全链路 UTF-8：nick 直接转 UTF-8 字节写入 struct（strncpy_s + _TRUNCATE 安全截断）
    {
        QByteArray ba = nick.toUtf8();
        strncpy_s(addFriRq.frinick, USER_NICK_LEN, ba.constData(), _TRUNCATE);
    }
    //发送给服务端
    qDebug()<<QString("服务端收到了来自%1(%2)添加 %3 的好友请求")
    .arg(addFriRq.myid).arg(addFriRq.mynick).arg(nick);

    m_pMediator->sendData((char*)&addFriRq,sizeof(addFriRq),19);
}

void Kernal::slots_addFriendTimer()
{
    qDebug()<<"ccc上线了，准备添加张三为好友！";

    PROT_ADD_FRIEND_RQ addFriRq(22);
    strcpy_s(addFriRq.mynick,USER_NICK_LEN,"ccc");
    strcpy_s(addFriRq.frinick,USER_NICK_LEN,"张三");
    DATA_RESCLIENT(addFriRq)


}

//朋友下线槽函数
void Kernal::slots_FriofflineTimer()
{
    PROT_FRIEND_OFFLINE Frioffline(10);  //李四下线

    DATA_RESCLIENT(Frioffline)


}


//发送下线通知
void Kernal::slots_sendMyoffline()
{
    //给服务端发送下线通知
    PROT_FRIEND_OFFLINE friOff(m_Mainwdiget->getid());
    m_pMediator->sendData((char*)&friOff,sizeof(friOff),1);


    //回收相关工作-------------------------------------------------------

#define SAFE_DELETE( POINT )\
    if(POINT)\
    {\
        delete POINT;\
        POINT = nullptr;\
    }


    SAFE_DELETE(m_pLogin)
    SAFE_DELETE(m_Mainwdiget)
    SAFE_DELETE(m_pUpateFriTimer)
    SAFE_DELETE(m_pAddFriendTimer)
    SAFE_DELETE(m_pFriofflineTimer)
}


void  Kernal::slots_quitLogin()
{
    SAFE_DELETE(m_pLogin)
    SAFE_DELETE(m_Mainwdiget)
    SAFE_DELETE(m_pUpateFriTimer)
    SAFE_DELETE(m_pAddFriendTimer)
    SAFE_DELETE(m_pFriofflineTimer)
}

//注册回复处理函数
void Kernal::deal_registerRs(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"注册成功：deal_registerRs";
    PROT_REGISTER_RS *pRegisterRs = (PROT_REGISTER_RS*)pbuf;
    if(pRegisterRs->result == REGISTER_SUCC)
    {
        QMessageBox::information(nullptr,"提示","注册成功!");
    }else if(pRegisterRs->result == REGISTER_TEL_EXIT)
    {
        QMessageBox::information(nullptr,"提示","注册失败:电话号码已存在！");
    }else if(pRegisterRs->result == REGISTER_NICK_EXIT)
    {
        QMessageBox::information(nullptr,"提示","注册失败:昵称已存在！");
    }
}

//登录回复处理函数
void Kernal::deal_loginRs(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"登录成功：deal_loginRs";
    PROT_LOGIN_RS *pLoginRs = (PROT_LOGIN_RS*)pbuf;
    if(pLoginRs->result == LOGIN_SUCCESS)
    {
        qDebug()<<"登录成功！";
        //存储用户id
        m_Mainwdiget->setmyid(pLoginRs->userid);

        //弹出显示主聊天窗口
        m_Mainwdiget->show();

        //隐藏登录窗口
        m_pLogin->hide();
    }
    else if(pLoginRs->result == LOGIN_NOTEXIT)
    {
        QMessageBox::warning(nullptr,"警告","用户不存在！");
    }
    else if(pLoginRs->result == LOGIN_PASSERROR)
    {
        QMessageBox::warning(nullptr,"警告","密码错误！");
    }

}

void Kernal::deal_friendInfo(char*pbuf,int len,unsigned long form)
{
    PROT_FRIEND_INFO *pFriend = (PROT_FRIEND_INFO *)pbuf;
    //全链路 UTF-8：struct 内 char[] 直接是 UTF-8 字节
    QString nick = QString::fromUtf8(pFriend->nick);
    QString feeling = QString::fromUtf8(pFriend->feeling);
    if(pFriend->userid == m_Mainwdiget->getid())
    {
        //说明是用户自己，返回用户自己的信息
        m_Mainwdiget->setMyInfo(pFriend->iconid,nick,feeling);
    }
    else
    {
        m_Mainwdiget->setFriendInfo(pFriend->userid,pFriend->iconid,pFriend->status,nick,feeling);
    }


}

void Kernal::deal_chatInfoRs(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"deal_chatInfoRs";

    PROT_CHAT_INFO_RS *pchatInfo = (PROT_CHAT_INFO_RS*)pbuf;
    if(pchatInfo->result == CHAT_RESULT_SUCC)
    {
        m_Mainwdiget->setFriendChatRs(pchatInfo->myid,"<font color = 'gray'  size = '2'>已送达</font>"  );   //回复中的myid是朋友，friid是自己
    }
    else if(pchatInfo->result == CHAT_RESULT_FAIL)
    {
        m_Mainwdiget->setFriendChatRs(pchatInfo->myid,"<font color = 'gray'  size = '2'>发送成功(对方已离线)</font>"  );
    }

}

void Kernal::deal_chatInfoRq(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"deal_chatInfoRq";
    PROT_CHAT_INFO_RQ *pchatInfoRq = (PROT_CHAT_INFO_RQ*)pbuf;

    //全链路 UTF-8：struct 内 char[] 直接是 UTF-8 字节
    QString msg = QString::fromUtf8(pchatInfoRq->msg);

    m_Mainwdiget->setFriendChatRs(pchatInfoRq->myid,QString("<p><font color = 'black' size = '4' font-weight:bold >%2</font></p>").arg(msg));
}

void Kernal::deal_addFriRs(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"deal_addFriRs";
    PROT_ADD_FRIEND_RS* paddFriRs = (PROT_ADD_FRIEND_RS*)pbuf;
    //全链路 UTF-8：struct 内 char[] 直接是 UTF-8 字节
    QString mynick = QString::fromUtf8(paddFriRs->mynick);
    if(paddFriRs->result == ADD_FRIEND_AGREE)
    {
        QMessageBox::information(m_Mainwdiget,"提示",QString("%1同意了你的好友请求！").arg(paddFriRs->mynick));

    }else if(paddFriRs->result == ADD_FRIEND_REJECT)
    {
        QMessageBox::information(m_Mainwdiget,"提示",QString("%1拒绝了你的好友请求！").arg(paddFriRs->mynick));

    }else if(paddFriRs->result == ADD_FRIEND_OFFLINE)
    {
        QMessageBox::information(m_Mainwdiget,"提示",QString("%1处于离线状态，请稍后添加！").arg(mynick));

    }else if(paddFriRs->result == ADD_FRIEND_NOTEXIT)
    {
        QMessageBox::information(m_Mainwdiget,"提示",QString("%1用户名称不存在！").arg(mynick));

    }
}

void Kernal::deal_addFriRq(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"deal_addFriRq";

    PROT_ADD_FRIEND_RQ* paddFriRq = (PROT_ADD_FRIEND_RQ*)pbuf;

    QMessageBox::StandardButton but = QMessageBox::information(nullptr,"提示",QString("%1请求添加你为好友，是否同意？").arg(paddFriRq->mynick),QMessageBox::Yes|QMessageBox::No);

    if(but == QMessageBox::Yes)
    {
        //回复对方同意
        PROT_ADD_FRIEND_RS addFriRs(ADD_FRIEND_AGREE,paddFriRq->myid,m_Mainwdiget->getid());
        strcpy_s(addFriRs.mynick,USER_NICK_LEN,m_Mainwdiget->getmyNick().toStdString().c_str());
        strcpy_s(addFriRs.destnick,USER_NICK_LEN,paddFriRq->mynick);

        //服务端模拟，把添加好友结果发给服务端
        qDebug()<<QString("收到了%1(%2)回复给%3(%4)的同意添加好友的请求！")
                  .arg(addFriRs.myid).arg(addFriRs.mynick).arg(addFriRs.destid).arg(addFriRs.destnick);
        m_pMediator->sendData((char*)&addFriRs,sizeof(addFriRs),19);

    }
    else if(but == QMessageBox::No)
    {
        PROT_ADD_FRIEND_RS addFriRs(ADD_FRIEND_REJECT,paddFriRq->myid,m_Mainwdiget->getid());
        strcpy_s(addFriRs.mynick,USER_NICK_LEN,m_Mainwdiget->getmyNick().toStdString().c_str());
        strcpy_s(addFriRs.destnick,USER_NICK_LEN,paddFriRq->mynick);

        //服务端模拟，把添加好友结果发给服务端
        qDebug()<<QString("收到了%1(%2)回复给%3(%4)的同意添加好友的请求,但被拒绝了")
                  .arg(addFriRs.myid).arg(addFriRs.mynick).arg(addFriRs.destid).arg(addFriRs.destnick);
        m_pMediator->sendData((char*)&addFriRs,sizeof(addFriRs),19);
    }
}

//朋友下线处理函数
void Kernal::deal_Frioffline(char*pbuf,int len,unsigned long form)
{
    qDebug()<<"deal_Frioffline";

    PROT_FRIEND_OFFLINE* pFriOff = (PROT_FRIEND_OFFLINE*)pbuf;

    m_Mainwdiget->setFriendOffline(pFriOff->offlineid);

}




















