#include <QApplication>
#include"kernal.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
//    LoginDia w;     //注释掉，改为在kernal中包含，并创建的
//    w.show();


    Kernal kernal;

    return a.exec();
}

//VS使用的是GB2312编码方式，QT使用的是UTF-8的编码方式
//GB2312编码方式使用char[]存储字符串，UTF-8使用QString存储字符串
//转码都在客户端做：QT发给VS之前转成GB2312编码，QT收到数据之前转成UTF-8编码
//需要转码的数据要满足两个条件：
//1、数据可能涉及中文
//2、数据需要在服务端使用

