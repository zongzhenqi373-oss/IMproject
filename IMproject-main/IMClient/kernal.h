#ifndef KERNAL_H
#define KERNAL_H

#include <QObject>
#include <QString>
#include "logindia.h"
#include "mainwdiget.h"
#include "client_core/ClientCore.h"

// 阶段0：kernal 重写为 client_core 的 Qt 适配层
// - 网络/协议/业务逻辑全部下沉到 im::ClientCore（纯 C++，可跨端复用）
// - 本类只做两件事：
//   1) UI 信号 → ClientCore 业务方法（槽函数）
//   2) ClientCore 回调（asio 网络线程）→ Qt 信号 → 排队到 UI 线程更新界面
// - 已删除：三个模拟服务端的测试定时器、net/mediator 层（由 client_core 内部 asio 取代）
class Kernal : public QObject, public im::IClientEvents
{
    Q_OBJECT
public:
    explicit Kernal(QObject *parent = nullptr);
    ~Kernal() override;

    // ---------------- im::IClientEvents（asio 网络线程触发，仅转发为 Qt 信号） ----------------
    void onRegisterResult(int result) override { emit sig_registerResult(result); }
    void onLoginResult(int result, int userId) override { emit sig_loginResult(result, userId); }
    void onSelfInfo(const im::UserInfo& info) override {
        emit sig_selfInfo(info.iconId, fromUtf8(info.nick), fromUtf8(info.feeling));
    }
    void onFriendInfo(const im::FriendInfo& info) override {
        emit sig_friendInfo(info.id, info.iconId, info.status,
                            fromUtf8(info.nick), fromUtf8(info.feeling));
    }
    void onChatMessage(int fromId, const std::string& msg) override {
        emit sig_chatMessage(fromId, fromUtf8(msg));
    }
    void onChatSendResult(int friId, int result) override { emit sig_chatSendResult(friId, result); }
    void onAddFriendRequest(int fromId, const std::string& fromNick) override {
        emit sig_addFriendRequest(fromId, fromUtf8(fromNick));
    }
    void onAddFriendResult(int result, const std::string& destNick) override {
        emit sig_addFriendResult(result, fromUtf8(destNick));
    }
    void onFriendOffline(int userId) override { emit sig_friendOffline(userId); }
    void onConnectionClosed() override { emit sig_connectionClosed(); }

signals:
    // core 事件 → UI 线程（queued）
    void sig_registerResult(int result);
    void sig_loginResult(int result, int userId);
    void sig_selfInfo(int iconId, QString nick, QString feeling);
    void sig_friendInfo(int friId, int iconId, int status, QString nick, QString feeling);
    void sig_chatMessage(int fromId, QString msg);
    void sig_chatSendResult(int friId, int result);
    void sig_addFriendRequest(int fromId, QString fromNick);
    void sig_addFriendResult(int result, QString destNick);
    void sig_friendOffline(int userId);
    void sig_connectionClosed();

public slots:
    // ---------------- UI 请求 → ClientCore（原 UI 信号对应的槽，签名保持不变） ----------------
    void slots_sendRegisterToServe(QString nick, QString tel, QString pass);
    void slots_sendLoginToServe(QString tel, QString pass);
    void slots_sendMsgtoServe(QString msg, int friid);
    void slots_addFriend(QString nick);
    void slots_sendMyoffline();
    void slots_quitLogin();

private:
    static QString fromUtf8(const std::string& value) {
        return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
    }

private slots:
    // ---------------- core 事件的 UI 线程处理（原 deal_xxx 逻辑） ----------------
    void onRegisterResultUi(int result);
    void onLoginResultUi(int result, int userId);
    void onSelfInfoUi(int iconId, QString nick, QString feeling);
    void onFriendInfoUi(int friId, int iconId, int status, QString nick, QString feeling);
    void onChatMessageUi(int fromId, QString msg);
    void onChatSendResultUi(int friId, int result);
    void onAddFriendRequestUi(int fromId, QString fromNick);
    void onAddFriendResultUi(int result, QString destNick);
    void onFriendOfflineUi(int userId);
    void onConnectionClosedUi();

private:
    void destroyUi();

    LoginDia* m_pLogin;
    mainwdiget* m_Mainwdiget;
    im::ClientCore m_core;
};

#endif // KERNAL_H
