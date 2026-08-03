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
void TCPClientmediator::transmitData(char* data, int len, SOCKET from)
{
    emit signals_recvServerData(data,len,from);
}

//信号与槽机制：通知和发送数据（步骤）
//1、两个类都必须继承自QObject，并且有Q_OBJECT宏
//2、在发送数据的类的头文件中使用signals声明信号；返回值必须是void，参数就是要传输的数据
//信号以signals_xxx命名，信号不是函数不用实现，需要在发送信号的地方使用emit或者Q_EMIT+发送的信号或者数据
//3、在接收数据的类的头文件中使用public slots声明槽函数，返回值和参数与所要连接的信号保持一致
//槽函数的名字一般以slots_xxx命名，名字与信号保持一致，槽函数需要实现
//4、在接受数据的类中，发送信号的对象new出来以后，连接信号和槽函数connect()。



















