#include"TCPClientmediator.h"
#include"../net/TCPClient.h"
#include"INetmediator.h"


TCPClientmediator::TCPClientmediator()
{
	m_pNet = new TCPClient(this);
}
TCPClientmediator::~TCPClientmediator()
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
bool TCPClientmediator::openNet()
{
	return m_pNet->initNet();
}

//关闭网络
void TCPClientmediator::closeNet()
{
	m_pNet->unInitNet();
}

//发送数据
bool TCPClientmediator::sendData(char* data, int len, unsigned long to)
{
	return m_pNet->sendData(data, len, to);
}


//传输数据给kernel
void TCPClientmediator::transmitData(char* data, int len, unsigned long from)
{
	//测试代码，打印一下新空间数据
	cout << "TCPClientmediator::transmitData:" << data << endl;
}