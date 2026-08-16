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
bool TCPServermediator::sendData(char* data, int len, NetEndpoint to)
{
	return m_pNet->sendData(data, len, to);
}


//传输数据给kernel
void TCPServermediator::transmitData(char* data, int len, NetEndpoint from)
{
	//关停时序防御：Kernel 析构会先 join 线程再置空 m_pKernel，此处判空双保险
	if (Kernel::m_pKernel) Kernel::m_pKernel->DealData(data, len, from);
}

//业务层请求关闭指定连接（转发给 net 层执行）
void TCPServermediator::closeConnection(NetEndpoint sock)
{
	m_pNet->closeConnection(sock);
}

//net 层上报连接断开（转发给 kernel）
void TCPServermediator::notifyDisconnect(NetEndpoint sock)
{
	if (Kernel::m_pKernel) Kernel::m_pKernel->DealDisconnect(sock);
}