#pragma once
#include"INetmediator.h"
#include"../Kernel.h"


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
	bool sendData(char* data, int len, NetEndpoint to);

	//传输数据给kernel
	void transmitData(char* data, int len, NetEndpoint from);

	//业务层请求关闭指定连接（转发给 net 层执行）
	void closeConnection(NetEndpoint sock) override;

	//net 层上报连接断开（转发给 kernel）
	void notifyDisconnect(NetEndpoint sock) override;

};
