#include"UDPmediator.h"
#include"../net/UDP.h"
#include"INetmediator.h"


UDPmediator::UDPmediator()
{
	m_pNet = new UDP(this);
}
UDPmediator::~UDPmediator() 
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
bool UDPmediator::openNet()
{
	return m_pNet->initNet();
}

//关闭网络
void UDPmediator::closeNet()
{
	m_pNet->unInitNet();
}

//发送数据
bool UDPmediator::sendData(char* data, int len, NetEndpoint to)
{
	return m_pNet->sendData(data, len, to);
}


//传输数据给kernel
void UDPmediator::transmitData(char* data, int len, NetEndpoint from)
{
	// Protocol payload is binary protobuf data, not a null-terminated string.
	cout << "UDPmediator::transmitData length=" << len
		 << ", from=" << from << endl;
}
