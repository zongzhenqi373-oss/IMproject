#pragma once
#include<WinSock2.h>
#include<process.h>
#include<iostream>
#include<atomic>
#include"def.h"
#include"NetTypes.h"
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
	virtual bool sendData(char* data,int len,NetEndpoint to) = 0;
	//TCP协议中，socket决定了数据发给谁，socket是UINT类型
	//UDP协议中，ip决定了数据发给谁，ip是u_long类型
	
	//接收数据
	virtual void recvData() = 0;

	//请求关闭指定客户端连接（仅 TCPServer 实现）。
	//约定：socket 的关闭权独占于 net 层——业务层禁止直接 closesocket，
	//防止句柄值被 Winsock 复用后遭误关。
	virtual void closeConnection(NetEndpoint sock) {}

protected:
	SOCKET m_socket;
	HANDLE m_handle;
	//跨线程停止标志：主线程 unInitNet 写，accept/recv 线程读，必须原子语义
	std::atomic<bool> m_bRunning;
	INetmediator* m_mediator;
};
