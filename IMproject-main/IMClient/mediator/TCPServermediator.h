#pragma once
#include"INetmediator.h"


class TCPServermediator :public INetmediator {
public:
	TCPServermediator();
	~TCPServermediator();

	//打开网络
	//返回值：bool true代表成功，false代表失败
	bool openNet();

	//关闭网络
	void closeNet();

	//发送数据
    bool sendData(char* data, int len, unsigned long to);

	//传输数据给kernel
    void transmitData(char* data, int len, SOCKET from);

};
