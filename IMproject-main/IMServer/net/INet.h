#pragma once
#include<WinSock2.h>
#include<process.h>
#include<iostream>
#include"def.h"
using namespace std;

#pragma comment(lib,"Ws2_32.lib")

class INetmediator;
class INet {
public:
	INet():m_socket(INVALID_SOCKET),m_handle(nullptr),m_bRunning(true), m_mediator(nullptr){}
	virtual ~INet() {}

	//初始化网络
	//返回值：bool true代表成功，false代表失败
	virtual bool initNet() = 0;

	//关闭网络
	virtual void unInitNet() = 0;

	//发送数据
	//data:要发送的数据
	//len:发送的数据长度
	//to：数据发给谁，在TCP协议中装socket，在UDP协议中装ip
	virtual bool sendData(char* data,int len,u_long to) = 0;
	//TCP协议中，socket决定了数据发给谁，socket是UINT类型
	//UDP协议中，ip决定了数据发给谁，ip是u_long类型
	
	//接收数据
	virtual void recvData() = 0;

protected:
	SOCKET m_socket;
	HANDLE m_handle;
	bool m_bRunning;
	INetmediator* m_mediator;
};