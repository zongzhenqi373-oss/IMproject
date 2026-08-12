#pragma once
#include<iostream>
#include<map>
#include<mutex>
#include<string>
#include"mediator/INetmediator.h"
#include"net/def.h"
#include"MySQL/CMySql.h"
#include"im.pb.h"


using namespace std;

class Kernel
{
public:
	static Kernel* m_pKernel;
	Kernel();
	~Kernel();

	//初始化函数指针数组
	void setFunArr();

	//打开服务器
	bool startServer();
	//关闭服务器
	void closeServer();

	//处理和分发所有收到的数据
	void DealData(char* data, int len, unsigned long from);

	//处理注册请求的函数
	void DealRegisterRq(char* data, int len, unsigned long from);

	//处理登录请求的函数
	void DealLoginRq(char* data, int len, unsigned long from);

	//根据id查询当前用户以及好友的信息
	void getUserInfoAndFriendInfo(int id);

	//根据id查询用户信息（填充 pb 消息）
	void getInfoById(int id, im::proto::FriendInfo* info);

	//组装并发送一个完整包体：4B 小端协议号 + pb payload
	void sendPacket(protType type, const std::string& payload, unsigned long to);

	//离线消息发送
	void sendOfflinemsg(int id);

	//处理下线请求
	void DealOfflineRq(char* data, int len, unsigned long from);

	//处理聊天请求
	void DealChatRq(char* data, int len, unsigned long from);

	//处理添加好友请求
	void DealAddFriendRq(char* data, int len, unsigned long from);

	//处理添加好友回复
	void DealAddFriendRs(char* data, int len, unsigned long from);


private:
	INetmediator* m_pMediator;
	//定义函数指针
	using DEAL_FUN = void(Kernel::*)(char*, int, unsigned long);
	//函数指针数组
	DEAL_FUN m_dealFunArr[DEF_PROT_COUNT];

	//数据库对象
	CMySql m_mysql;
	map<int, unsigned long> m_mapIdtoSocket;

	//保护 m_mapIdtoSocket 的并发访问（recvThread 在不同连接的线程中并发调用 DealData）
	//阶段-1 仅加锁消除数据竞争；worker 线程池推迟到阶段0与 asio 重写合并
	mutex m_mapIdtoSocketMutex;

	//m_mapIdtoSocket 的加锁辅助方法（锁内查询/修改，锁外使用，避免持锁调用 sendData）
	//获取 id 对应的 socket，存在则返回 true 并写入 out
	bool getSocket(int id, unsigned long& out);
	//设置 id → socket 映射
	void setSocket(int id, unsigned long sock);
	//判断 id 是否在线（存在映射）
	bool isOnline(int id);
	//删除 id 的映射，并返回被删除的 socket（用于后续 closesocket）
	bool eraseSocket(int id, unsigned long& outSock);
};

