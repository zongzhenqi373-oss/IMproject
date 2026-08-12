#include "kernal.h"
#include <QDebug>
#include <QMessageBox>

Kernal::Kernal(QObject *parent)
    : QObject(parent)
    , m_pLogin(new LoginDia)
    , m_Mainwdiget(new mainwdiget)
{
    // core 事件 → UI 线程槽（显式排队连接：回调来自 asio 网络线程）
    m_core.setEventSink(this);
    connect(this, &Kernal::sig_registerResult,   this, &Kernal::onRegisterResultUi,   Qt::QueuedConnection);
    connect(this, &Kernal::sig_loginResult,      this, &Kernal::onLoginResultUi,      Qt::QueuedConnection);
    connect(this, &Kernal::sig_selfInfo,         this, &Kernal::onSelfInfoUi,         Qt::QueuedConnection);
    connect(this, &Kernal::sig_friendInfo,       this, &Kernal::onFriendInfoUi,       Qt::QueuedConnection);
    connect(this, &Kernal::sig_chatMessage,      this, &Kernal::onChatMessageUi,      Qt::QueuedConnection);
    connect(this, &Kernal::sig_chatSendResult,   this, &Kernal::onChatSendResultUi,   Qt::QueuedConnection);
    connect(this, &Kernal::sig_addFriendRequest, this, &Kernal::onAddFriendRequestUi, Qt::QueuedConnection);
    connect(this, &Kernal::sig_addFriendResult,  this, &Kernal::onAddFriendResultUi,  Qt::QueuedConnection);
    connect(this, &Kernal::sig_friendOffline,    this, &Kernal::onFriendOfflineUi,    Qt::QueuedConnection);
    connect(this, &Kernal::sig_connectionClosed, this, &Kernal::onConnectionClosedUi, Qt::QueuedConnection);

    // 连接服务端（原 TCPClient 硬编码 127.0.0.1，行为保持一致）
    if (!m_core.connectToServer("127.0.0.1")) {
        QMessageBox::about(m_pLogin, "提示！", "打开网络失败！");
        exit(-1);
    }

    // UI 信号 → core 业务方法
    connect(m_pLogin, SIGNAL(signals_sendRegisterInfo(QString,QString,QString)),
            this, SLOT(slots_sendRegisterToServe(QString,QString,QString)));
    connect(m_pLogin, SIGNAL(signals_sendLoginInfo(QString,QString)),
            this, SLOT(slots_sendLoginToServe(QString,QString)));
    connect(m_pLogin, SIGNAL(signals_quitLogin()), this, SLOT(slots_quitLogin()));
    connect(m_Mainwdiget, SIGNAL(signals_sendMsgAndIdtoKernal(QString,int)),
            this, SLOT(slots_sendMsgtoServe(QString,int)));
    connect(m_Mainwdiget, SIGNAL(signals_addFriend(QString)), this, SLOT(slots_addFriend(QString)));
    connect(m_Mainwdiget, SIGNAL(signals_sendMyoffline()), this, SLOT(slots_sendMyoffline()));

    m_pLogin->show();
}

Kernal::~Kernal()
{
    // 先摘掉回调再断连，避免析构过程中触发 Qt 信号
    m_core.setEventSink(nullptr);
    m_core.disconnect();
    destroyUi();
}

void Kernal::destroyUi()
{
    if (m_pLogin)      { delete m_pLogin;      m_pLogin = nullptr; }
    if (m_Mainwdiget)  { delete m_Mainwdiget;  m_Mainwdiget = nullptr; }
}

// ==================== UI 请求 → ClientCore ====================

void Kernal::slots_sendRegisterToServe(QString nick, QString tel, QString pass)
{
    m_core.sendRegister(nick.toUtf8().toStdString(), tel.toStdString(), pass.toStdString());
}

void Kernal::slots_sendLoginToServe(QString tel, QString pass)
{
    m_core.sendLogin(tel.toStdString(), pass.toStdString());
}

void Kernal::slots_sendMsgtoServe(QString msg, int friid)
{
    m_core.sendChatMessage(friid, msg.toUtf8().toStdString());
}

void Kernal::slots_addFriend(QString nick)
{
    m_core.sendAddFriendRequest(nick.toUtf8().toStdString());
}

void Kernal::slots_sendMyoffline()
{
    m_core.sendOfflineNotify();
    m_core.disconnect(); // transport 内部会等发送队列排空再关 socket
    destroyUi();
}

void Kernal::slots_quitLogin()
{
    m_core.disconnect();
    destroyUi();
}

// ==================== core 事件的 UI 线程处理 ====================

void Kernal::onRegisterResultUi(int result)
{
    if (result == im::proto::REGISTER_SUCC) {
        QMessageBox::information(nullptr, "提示", "注册成功!");
    } else if (result == im::proto::REGISTER_TEL_EXIT) {
        QMessageBox::information(nullptr, "提示", "注册失败:电话号码已存在！");
    } else if (result == im::proto::REGISTER_NICK_EXIT) {
        QMessageBox::information(nullptr, "提示", "注册失败:昵称已存在！");
    }
}

void Kernal::onLoginResultUi(int result, int userId)
{
    if (result == im::proto::LOGIN_SUCCESS) {
        m_Mainwdiget->setmyid(userId);
        m_Mainwdiget->show();
        m_pLogin->hide();
    } else if (result == im::proto::LOGIN_NOTEXIT) {
        QMessageBox::warning(nullptr, "警告", "用户不存在！");
    } else if (result == im::proto::LOGIN_PASSERROR) {
        QMessageBox::warning(nullptr, "警告", "密码错误！");
    }
}

void Kernal::onSelfInfoUi(int iconId, QString nick, QString feeling)
{
    m_Mainwdiget->setMyInfo(iconId, nick, feeling);
}

void Kernal::onFriendInfoUi(int friId, int iconId, int status, QString nick, QString feeling)
{
    m_Mainwdiget->setFriendInfo(friId, iconId, status, nick, feeling);
}

void Kernal::onChatMessageUi(int fromId, QString msg)
{
    m_Mainwdiget->setFriendChatRs(fromId,
        QString("<p><font color = 'black' size = '4' font-weight:bold >%2</font></p>").arg(msg));
}

void Kernal::onChatSendResultUi(int friId, int result)
{
    if (result == im::proto::CHAT_RESULT_SUCC) {
        m_Mainwdiget->setFriendChatRs(friId, "<font color = 'gray'  size = '2'>已送达</font>");
    } else if (result == im::proto::CHAT_RESULT_FAIL) {
        m_Mainwdiget->setFriendChatRs(friId, "<font color = 'gray'  size = '2'>发送成功(对方已离线)</font>");
    }
}

void Kernal::onAddFriendRequestUi(int fromId, QString fromNick)
{
    QMessageBox::StandardButton but = QMessageBox::information(
        nullptr, "提示", QString("%1请求添加你为好友，是否同意？").arg(fromNick),
        QMessageBox::Yes | QMessageBox::No);
    m_core.answerAddFriend(fromId, fromNick.toUtf8().toStdString(), but == QMessageBox::Yes);
}

void Kernal::onAddFriendResultUi(int result, QString destNick)
{
    if (result == im::proto::ADD_FRIEND_AGREE) {
        QMessageBox::information(m_Mainwdiget, "提示", QString("%1同意了你的好友请求！").arg(destNick));
    } else if (result == im::proto::ADD_FRIEND_REJECT) {
        QMessageBox::information(m_Mainwdiget, "提示", QString("%1拒绝了你的好友请求！").arg(destNick));
    } else if (result == im::proto::ADD_FRIEND_OFFLINE) {
        QMessageBox::information(m_Mainwdiget, "提示", QString("%1处于离线状态，请稍后添加！").arg(destNick));
    } else if (result == im::proto::ADD_FRIEND_NOTEXIT) {
        QMessageBox::information(m_Mainwdiget, "提示", QString("%1用户名称不存在！").arg(destNick));
    }
}

void Kernal::onFriendOfflineUi(int userId)
{
    m_Mainwdiget->setFriendOffline(userId);
}

void Kernal::onConnectionClosedUi()
{
    qDebug() << "与服务端连接已断开";
}
