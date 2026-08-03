#pragma once
#include <winsock2.h>
#include <QObject>

class INet;
class INetmediator :public QObject{
    Q_OBJECT
public:
	INetmediator():m_pNet(nullptr){}
	virtual ~INetmediator() {}

	//打开网络
	//返回值：bool true代表成功，false代表失败
	virtual bool openNet() = 0;

	//关闭网络
	virtual void closeNet() = 0;

	//发送数据
    virtual bool sendData(char* data, int len, unsigned long to) = 0;

	//传输数据给kernel
    virtual void transmitData(char* data, int len, SOCKET from) = 0;

protected:
	INet* m_pNet;

};
