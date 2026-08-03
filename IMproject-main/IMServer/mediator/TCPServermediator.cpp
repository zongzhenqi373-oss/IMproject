#include"TCPServermediator.h"
#include"../net/TCPServer.h"
#include"INetmediator.h"

TCPServermediator::TCPServermediator()
{
	m_pNet = new TCPServer(this);
}
TCPServermediator::~TCPServermediator()
{
	if (m_pNet)
	{
		m_pNet->unInitNet();
		delete m_pNet;
		m_pNet = nullptr;
	}
}

//打开网络
//返回值：bool true代表成功，false代表失败
bool TCPServermediator::openNet()
{
	return m_pNet->initNet();
}

//关闭网络
void TCPServermediator::closeNet()
{
	m_pNet->unInitNet();
}

//发送数据
bool TCPServermediator::sendData(char* data, int len, unsigned long to)
{
	return m_pNet->sendData(data, len, to);
}


//传输数据给kernel
void TCPServermediator::transmitData(char* data, int len, unsigned long from)
{
	Kernel::m_pKernel->DealData(data, len, from);
}