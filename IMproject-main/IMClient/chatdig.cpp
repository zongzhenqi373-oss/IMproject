#include "chatdig.h"
#include "ui_chatdig.h"
#include<QMessageBox>
#include<QTime>


chatdig::chatdig(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::chatdig)
{
    ui->setupUi(this);

    connect(ui->pb_send,SIGNAL(clicked()),this,SLOT(slots_sendMsg()));
    connect(ui->pb_clear,SIGNAL(clicked()),this,SLOT(slots_clearMsg()));

}

chatdig::~chatdig()
{
    qDebug()<<"chatdig";

    delete ui;
}

void chatdig::setMsg(QString s)
{
    ui->tB_rs->append(s);

}

void chatdig::slots_sendMsg()
{
    QString Pmsg = ui->te_inword->toPlainText();   //获取纯文本

    if(Pmsg.isEmpty())
    {
        QMessageBox::information(this,"提示：","聊天内容不能为空");
        return;
    }

    QString Hmsg = ui->te_inword->toHtml();//获取带html格式的文本

    QString s = QString("<font color = 'gray' size = '2' >我[%1]</font><p><font color = 'black' size = '4' font-weight:bold >%2</font></p>")
            .arg(QTime::currentTime().toString("hh:mm:ss")).arg(Hmsg);
    //将自己的聊天内容显示到聊天记录中
    ui->tB_rs->append(s);

    //向上层发送聊天内容
    emit signals_sendMsg(Pmsg);   //发送信号给friendItem


    //清空聊天内容
    slots_clearMsg();


}
void chatdig::slots_clearMsg()
{
    ui->te_inword->clear();//清空里面的文字
}
