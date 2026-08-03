#ifndef MAINWDIGET_H
#define MAINWDIGET_H

#include <QWidget>
#include"Frienditem.h"
#include<map>
#include<QMenu>
using namespace std;

namespace Ui {
class mainwdiget;
}

class mainwdiget : public QWidget
{
    Q_OBJECT

public:
    explicit mainwdiget(QWidget *parent = nullptr);
    ~mainwdiget();
public:
    void setmyid(int id){m_myid = id;}
    int getid(){return m_myid;}
    void setMyImg(int iconid);
    void setMyInfo(int iconid,QString nick,QString feeling);
    void setFriendInfo(int fril,int iconid,int status,QString nick,QString feeling);
    void setFriendChatRs(int friid,QString msg);
    QString getmyNick(){return m_nick;};
    int geticonid(){return m_iconid;}
    QString getmyFeeling(){return m_feeling;};

    void setFriendOffline(int friid);

public:
    virtual void closeEvent(QCloseEvent *event)override;


private:
    Ui::mainwdiget *ui;
    int m_myid;
    int m_iconid;
    QString m_nick;
    QString m_feeling;


    map<int , FriendItem*> m_friendItemMap; //存储好友的列表

    QMenu *m_pMenu;

signals:
    void signals_sendMsgAndIdtoKernal(QString,int);
    void signals_addFriend(QString);
    void signals_sendMyoffline();

public slots:
    void slots_showMenu();
    void slots_dealMenu(QAction*);
};



#endif // MAINWDIGET_H
