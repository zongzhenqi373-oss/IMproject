#ifndef CHATDIG_H
#define CHATDIG_H

#include <QWidget>

namespace Ui {
class chatdig;
}

class chatdig : public QWidget
{
    Q_OBJECT

public:
    explicit chatdig(QWidget *parent = nullptr);
    ~chatdig();

    void setMsg(QString s);

private:
    Ui::chatdig *ui;


public slots:
    void slots_sendMsg();
    void slots_clearMsg();


signals:
    void signals_sendMsg(QString);
};

#endif // CHATDIG_H
