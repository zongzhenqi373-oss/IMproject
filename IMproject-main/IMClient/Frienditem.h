#ifndef FRIENDITEM_H
#define FRIENDITEM_H

#include <QWidget>
#include"chatdig.h"

namespace Ui {
class FriendItem;
}

class FriendItem : public QWidget
{
    Q_OBJECT

public:
    explicit FriendItem(QWidget *parent = nullptr);
    ~FriendItem();

public:
    void setInfo(int ,int, QString,QString);
    void setid(int );
    void setChatMsg(QString msg);
    QString getNick(){return m_nick;}
    void setOffline();


public slots:
    void slots_showChatdig();
    void slots_recvMsg(QString);


private:
    Ui::FriendItem *ui;

    //存储朋友信息
    int m_id;
    int m_iconid;
    int m_status;
    QString m_nick;
    QString m_feeling;
    chatdig* m_chatdig;


signals:
    void signals_sendMsgAndId(QString,int);

};

#endif // FRIENDITEM_H
