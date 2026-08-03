#pragma once
#include"INet.h"

class TCPClient :public INet {
public:
	TCPClient(INetmediator* p);
	~TCPClient();

	//初始化网络
	bool initNet();

	//关闭网络
	void unInitNet();

	//发送数据
	bool sendData(char* data, int len, u_long to);

	//接收数据
	void recvData();

	//接收数据的线程函数
	static unsigned __stdcall recvThread(void* lpVoid);


};