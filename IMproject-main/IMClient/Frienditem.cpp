#include "Frienditem.h"
#include "ui_Frienditem.h"
#include"../IMClient/net/def.h"
#include<QBitmap>
#include"chatdig.h"
#include<QDebug>
#include<QTime>

FriendItem::FriendItem(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::FriendItem),
    m_chatdig(new chatdig)
{
    ui->setupUi(this);

    connect(ui->pb_img,SIGNAL(clicked()),this,SLOT(slots_showChatdig()));

    connect(m_chatdig,SIGNAL(signals_sendMsg(QString)),this,SLOT(slots_recvMsg(QString)));
}

FriendItem::~FriendItem()
{
    qDebug()<<"FriendItem";
    delete ui;

    if(m_chatdig)
    {
        delete m_chatdig;
        m_chatdig = nullptr;
    }
}


void FriendItem::setInfo(int iconid,int status, QString nick,QString feeling)
{
    //设置昵称
    ui->l_nick->setText(nick);
    //设置个性签名
    ui->l_feeling->setText(feeling);
    //设置头像
    if(status == STATUS_ONLINE)  //如果是在线
    {
        ui->pb_img->setIcon(QIcon(QString(":/images/tx/%1.png").arg(iconid)));
    }
    else if(status == STATUS_OFFLINE)   //如果是离线，去色变黑白
    {
        ui->pb_img->setIcon(QBitmap(QString(":/images/tx/%1.png").arg(iconid)));
        ui->l_nick->setStyleSheet("color: gray;");
        ui->l_feeling->setStyleSheet("color: gray;");
    }

    //存储信息
    m_iconid = iconid;
    m_status = status;
    m_nick = nick;
    m_feeling = feeling;


}
void FriendItem::setid(int friid)
{
    m_id = friid;
}

void FriendItem::setChatMsg(QString msg)
{
   QString s = QString("<font color = 'gray' size = '2'>%1[%2]</font>%3").arg(m_nick).arg(QTime::currentTime().toString("hh:mm:ss")).arg(msg);

   m_chatdig->setMsg(s);

}

void FriendItem::slots_showChatdig()
{
    m_chatdig->setWindowTitle(QString("与%1的聊天").arg(m_nick));
    m_chatdig->show();  //显示聊天窗口

}

void FriendItem::slots_recvMsg(QString Msg)
{
    qDebug()<<"slots_recvMsg(QString Msg)";

    //除了聊天内容之外，再加上朋友id
    emit signals_sendMsgAndId(Msg,m_id);   //发送信号给mainwdiget
}

void FriendItem::setOffline()
{
     ui->pb_img->setIcon(QBitmap(QString(":/images/tx/%1.png").arg(m_iconid)));
     ui->l_nick->setStyleSheet("color: gray;");
     ui->l_feeling->setStyleSheet("color: gray;");
}
