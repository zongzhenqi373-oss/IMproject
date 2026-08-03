#pragma once
#include"INet.h"
#include"../mediator/INetmediator.h"

class UDP :public INet {
public:
	UDP(INetmediator* p);
	~UDP();

	//初始化网络
	//返回值：bool true代表成功，false代表失败
	bool initNet();

	//关闭网络
	void unInitNet();

	//发送数据
	//data:要发送的数据
	//len:发送的数据长度
	//to：数据发给谁，在TCP协议中装socket，在UDP协议中装ip
	bool sendData(char* data, int len, u_long to);
	//TCP协议中，socket决定了数据发给谁，socket是UINT类型
	//UDP协议中，ip决定了数据发给谁，ip是u_long类型

	//接收数据
	void recvData();

	//接收数据的线程函数
	static unsigned __stdcall recvThread(void* lpVoid);
};