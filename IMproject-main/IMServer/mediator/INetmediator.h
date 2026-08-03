#pragma once
#include<iostream>
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
	virtual bool sendData(char* data, int len, unsigned long to) = 0;

	//传输数据给kernel
	virtual void transmitData(char* data, int len, unsigned long to) = 0;

protected:
	INet* m_pNet;

};