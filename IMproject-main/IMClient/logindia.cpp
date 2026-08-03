#include "logindia.h"
#include "ui_logindia.h"
#include<QMessageBox>
#include<QDebug>

#define CHECK_TEL(telzujian) QString tel = ui->telzujian->text().trimmed();\
                            if(tel.size()!=11)\
                            {\
                                QMessageBox::warning(this,"警告!","电话号码长度过长或者过短！");\
                                return;\
                            }\
                            for(int i = 0;i<11;i++)\
                            {\
                                if(tel[i]<'0' || tel[i]>'9')\
                                {\
                                    QMessageBox::warning(this,"警告!","电话号码必须为数字！");\
                                    return;\
                                }\
                            }


LoginDia::LoginDia(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LoginDia)
{
    ui->setupUi(this);

    //点击注册的处理，绑定链接
    connect(ui->pb_register,SIGNAL(clicked()),this,SLOT(slots_register()));
    connect(ui->pb_register_clear,SIGNAL(clicked()),this,SLOT(slots_registerClear()));

    //点击登录的处理，绑定链接
    connect(ui->pb_login,SIGNAL(clicked()),this,SLOT(slots_login()));
    connect(ui->pb_clear,SIGNAL(clicked()),this,SLOT(slots_clear()));
}

LoginDia::~LoginDia()
{
    qDebug()<<"LoginDia";
    delete ui;
}

//点击注册函数
void LoginDia::slots_register()
{
    QString nick = ui->le_nick->text().trimmed(); //去除两边的空白符
    if(nick.isEmpty())
    {
        QMessageBox::warning(this,"警告!","昵称不能为空！");
        return;
    }

    /*QString tel = ui->le_register_tel->text().trimmed(); //去除两边的空白符
    if(tel.size()!=11)
    {
        QMessageBox::warning(this,"警告!","电话号码长度过长或者过短！");
        return;
    }

    for(int i = 0;i<11;i++)
    {
        if(tel[i]<'0' || tel[i]>'9')
        {
            QMessageBox::warning(this,"警告!","电话号码必须为数字！");
            return;
        }
    }*/

    CHECK_TEL(le_register_tel);


    //手机号校验完成，判断两次密码的输入是否一致
    QString rpass = ui->le_register_pass->text().trimmed();
    QString passAgain = ui->le_pass_again->text().trimmed();

    if(rpass.isEmpty())
    {
        QMessageBox::warning(this,"警告!","密码不能为空！");
        return;
    }

    if(rpass!=passAgain)
    {
        QMessageBox::warning(this,"警告!","两次密码输入不一致！");
        return;
    }

    //QMessageBox::information(this,"注册","注册成功,点击ok返回！");
    //可以实现一个新窗口，注册完成后询问是否返回登录界面，然后在新窗口中添加两个按钮，确定按下后显示登录界面，取消按下后清空继续注册

    /*---*/

    //注册信息，发送信号，发送给kernal
    emit signals_sendRegisterInfo(nick,tel,rpass);



}


//注册清空
void LoginDia::slots_registerClear()
{
    ui->le_nick->clear();
    ui->le_register_tel->clear();
    ui->le_register_pass->clear();
    ui->le_pass_again->clear();
}


void LoginDia::slots_login()
{
    CHECK_TEL(le_tel);

    QString lpass = ui->le_pass->text().trimmed();
    if(lpass.isEmpty())
    {
        QMessageBox::warning(this,"警告!","密码不能为空！");
        return;
    }


    //登录信息，发送信号，发送给kernal
    emit signals_sendLoginInfo(tel,lpass);


}
void LoginDia::slots_clear()
{
    ui->le_tel->clear();
    ui->le_pass->clear();
}

void LoginDia::closeEvent(QCloseEvent *event)
{
    qDebug()<<" LoginDia::closeEvent ";
    emit signals_quitLogin();
}
