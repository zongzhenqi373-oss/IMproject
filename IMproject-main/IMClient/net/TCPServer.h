#pragma once

#include"INet.h"
#include<map>
#include<list>

class TCPServer :public INet {
public:
	TCPServer(INetmediator* p);
	~TCPServer();

	//初始化网络
	bool initNet();

	//关闭网络
	void unInitNet();

	//发送数据
	bool sendData(char* data, int len, u_long to);

	//接收数据
	void recvData();

	//接收连接的线程函数
	static unsigned __stdcall acceptThread(void* lpVoid);

	//接收数据的线程函数
	static unsigned __stdcall recvThread(void* lpVoid);
	
private:
	//定义一个保存服务端地址和对应套接字的图
	//保存线程id和对应的socket（一个和线程只能使用一个socket，因此一个线程只能接收一个客户端）
	map<unsigned int, SOCKET> m_addrFrom;
	//保存接收数据的线程句柄
	list<HANDLE> m_listHandle;
};