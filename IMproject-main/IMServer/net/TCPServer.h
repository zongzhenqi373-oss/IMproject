#pragma once

#include"INet.h"
#include<map>
#include<list>
#include<mutex>

class TCPServer :public INet {
public:
	TCPServer(INetmediator* p);
	~TCPServer();

	//初始化网络
	bool initNet();

	//关闭网络
	void unInitNet();

	//发送数据
	bool sendData(char* data, int len, NetEndpoint to);

	//接收数据
	void recvData(SOCKET s);

	//关闭指定客户端连接（override，业务层经 mediator 调用）。
	//仅当该 socket 的条目仍在 m_addrFrom 中时才关闭——条目在则连接存活且归本层管理；
	//条目不在则说明已被本层关闭，绝不再 closesocket，防止句柄值被复用后误关新连接。
	void closeConnection(NetEndpoint sock) override;

	//接收连接的线程函数
	static unsigned __stdcall acceptThread(void* lpVoid);

	//接收数据的线程函数
	static unsigned __stdcall recvThread(void* lpVoid);

private:
	//recvThread 启动参数（socket 直传，消除"先建线程后写 map"的启动竞争，取代 Sleep(5) hack）
	struct RecvCtx {
		TCPServer* self;
		SOCKET sock;
	};
	//定义一个保存服务端地址和对应套接字的图
	//保存线程id和对应的socket（一个和线程只能使用一个socket，因此一个线程只能接收一个客户端）
	map<unsigned int, SOCKET> m_addrFrom;
	//保护 m_addrFrom 的并发访问（acceptThread 写 / recvThread 读 / unInitNet 遍历）
	mutex m_addrFromMutex;
	//保存接收数据的线程句柄
	list<HANDLE> m_listHandle;
};
