#pragma once
#include<iostream>
#include"../net/NetTypes.h"
using namespace std;

class INet;
class INetmediator {
public:
	INetmediator():m_pNet(nullptr){}
	virtual ~INetmediator() {}

	//打开网络
	//返回值：bool true代表成功，false代表失败
	virtual bool openNet() = 0;

	//关闭网络
	virtual void closeNet() = 0;

	//发送数据
	virtual bool sendData(char* data, int len, NetEndpoint to) = 0;

	//传输数据给kernel
	virtual void transmitData(char* data, int len, NetEndpoint to) = 0;

	//业务层请求关闭指定连接（转发给 net 层执行；仅 TCPServermediator 实现）
	virtual void closeConnection(NetEndpoint sock) {}

	//net 层上报：连接已断开且 socket 已被 net 层关闭
	//（业务层据此清理 id→socket 映射并广播下线；仅 TCPServermediator 实现）
	virtual void notifyDisconnect(NetEndpoint sock) {}

protected:
	INet* m_pNet;

};
