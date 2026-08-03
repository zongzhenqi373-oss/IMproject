#include "mainwdiget.h"
#include "ui_mainwdiget.h"
#include<QSize>
#include<QInputDialog>
#include<QMessageBox>
#include <QDebug>

mainwdiget::mainwdiget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::mainwdiget)
{
    ui->setupUi(this);
    //创建一个菜单对象
    m_pMenu = new QMenu;
    m_pMenu->addAction("添加好友");
    m_pMenu->addAction("系统设置");


    connect(ui->pb_menu,SIGNAL(clicked()),this,SLOT(slots_showMenu()));

    connect(m_pMenu,SIGNAL(triggered(QAction*)),this,SLOT(slots_dealMenu(QAction*)));

}

mainwdiget::~mainwdiget()
{
    qDebug()<<"mainwdiget";
    delete ui;


    for(const pair<int , FriendItem*>&pr:m_friendItemMap)
    {
        if(pr.second)
        {
        delete pr.second;
        }
    }

    if(m_pMenu)
    {
        delete m_pMenu;
        m_pMenu = nullptr;
    }

}


void mainwdiget::setMyImg(int iconid)
{
    ui->pb_img->setIcon(QIcon(QString(":/images/tx/%1.png").arg(iconid)));

}
void mainwdiget::setMyInfo(int iconid,QString nick,QString feeling)
{
    //设置头像
    ui->pb_img->setIcon(QIcon(QString(":/images/tx/%1.png").arg(iconid)));
    //设置昵称
    ui->l_nick->setText(nick);
    //设置个性签名
    ui->le_feeling->setText(feeling);


    //存储自己信息
    m_iconid = iconid;
    m_nick = nick;
    m_feeling = feeling;
}

void mainwdiget::setFriendInfo(int friid,int iconid,int status,QString nick,QString feeling)
{
    //判断某个朋友是否存在
    if(m_friendItemMap.count(friid))//朋友已经存在
    {
        m_friendItemMap[friid]->setInfo(iconid,status,nick,feeling);
    }
    else    //朋友不存在, 需要创建
    {
        FriendItem * pfriItem = new FriendItem;

        //设置朋友的信息
        pfriItem->setInfo(iconid,status, nick,feeling );
        pfriItem->setid(friid);

        //添加到map中
        m_friendItemMap[friid] = pfriItem;

        QListWidgetItem*pitem = new QListWidgetItem;
        ui->lw_friendlist->addItem(pitem);
        ui->lw_friendlist->setItemWidget(pitem,pfriItem);   //放置朋友的窗口组件


        //调整表项大小
        QRect rect = pfriItem->geometry();

        //设置成和FriendItem一样的大小
        pitem->setSizeHint(QSize(rect.width(),rect.height()));

        //绑定FriendItem发送给mainwdiget聊天记录的信号
        connect(pfriItem,SIGNAL(signals_sendMsgAndId(QString,int)),
                this,SIGNAL(signals_sendMsgAndIdtoKernal(QString,int)));
    }
}

//设置回复状态
void mainwdiget::setFriendChatRs(int friid,QString msg)
{
    if(m_friendItemMap.count(friid))
    {
        m_friendItemMap[friid]->setChatMsg(msg);
    }
}

//显示菜单
void mainwdiget::slots_showMenu()
{
    //基于当前屏幕，获取鼠标的坐标点
    QPoint po = QCursor::pos();
    qDebug()<<"po: "<<po;

    //获取菜单的高度
    QSize size = m_pMenu->sizeHint();
    size.height();

    po.setY(796 -  size.height() - 11);//减去弹窗高度
    po.setX(2756);

    //显示菜单
    m_pMenu->exec(po);

}

//处理菜单
void mainwdiget::slots_dealMenu(QAction* paction)
{
    qDebug()<<"slots_dealMenu ";

    if(paction->text() == "添加好友")
    {
        QString nick = QInputDialog::getText(this,"添加好友","请输入好友昵称：");
        if(!nick.isEmpty())
        {
            //如果是自己
            if(nick == m_nick)
            {
                QMessageBox::warning(this,"警告","不能添加自己为好友！");
                return;
            }
            else
            {
                for(pair<int,FriendItem*> pr : m_friendItemMap)
                {
                    if(pr.second->getNick() == nick)
                    {
                        QMessageBox::warning(this,"警告","对方已经是你的好友了！");
                        return;
                    }
                }
            }

            //发射添加朋友的信号
            emit signals_addFriend(nick);
        }
    }


}

void mainwdiget::setFriendOffline(int friid)
{
    if(m_friendItemMap.count(friid))
    {
        m_friendItemMap[friid]->setOffline();
    }

}

void mainwdiget::closeEvent(QCloseEvent *event)
{
    qDebug()<<"mainwdiget::closeEvent ";

    emit signals_sendMyoffline();

}
