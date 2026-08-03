#ifndef LOGINDIA_H
#define LOGINDIA_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class LoginDia; }
QT_END_NAMESPACE

class LoginDia : public QWidget
{
    Q_OBJECT

public:
    LoginDia(QWidget *parent = nullptr);
    ~LoginDia();

    virtual void closeEvent(QCloseEvent *event)override;

private:
    Ui::LoginDia *ui;


public slots:
    void slots_register();
    void slots_registerClear();
    void slots_login();
    void slots_clear();

signals:
   void signals_sendRegisterInfo(QString,QString,QString);
   void signals_sendLoginInfo(QString,QString);
   void signals_quitLogin();


};
#endif // LOGINDIA_H
