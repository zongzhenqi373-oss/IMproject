#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include"UDP.h"
#include"../mediator/UDPmediator.h"


UDP::UDP(INetmediator* p)
{
	m_mediator = p;
}
UDP::~UDP()
{

}

//初始化网络：加载库，创建套接字，绑定，创建接收数据的线程
bool UDP::initNet()
{
	//注意：1、不用回收资源，失败直接return false；2、创建的套接字赋值给成员变量
	//1、加载库
	WORD version = MAKEWORD(2, 2);
	WSADATA data = {  };
	int err = WSAStartup(version, &data);
	if (err != 0)
	{
		cout << "UDP::WSAStartup fail:" << err << endl;
		return false;
	}

	//加载库成功，判断库的版本号是否正确
	if (HIBYTE(data.wVersion) == 2 && LOBYTE(data.wVersion) == 2)
	{
		cout << "UDP::WSAStartup success!" << endl;
	}
	else   //虽然加载库成功了，但是版本号不正确
	{
		cout << "UDP::WSAStartup error!" << endl;

		return false;
	}

	//2、创建套接字
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_socket == INVALID_SOCKET)
	{
		cout << "UDP::socket error:" << WSAGetLastError() << endl;
		return false;
	}
	else
	{
		cout << "UDP::socket success!" << endl;
	}

	//3、绑定
	sockaddr_in saddr;
	saddr.sin_family = AF_INET;
	//魔鬼数字：出现在代码中的数字,定义成宏
	saddr.sin_port = htons(UDP_PORT);
	saddr.sin_addr.S_un.S_addr = ADDR_ANY;
    err = ::bind(m_socket, (sockaddr*)&saddr, sizeof(saddr));
	if (err == SOCKET_ERROR)
	{
		cout << "UDP::bind error:" << WSAGetLastError()/*打印错误码*/ << endl;
		return false;
	}
	else
	{
		cout << "UDP::bind success!" << endl;
	}

	//4、创建接收数据的线程
	//CreateThread和ExitThread是一对，ExitThread在退出的时候不会回收资源
	//如果线程中使用C++运行时库的函数，例如strcpy，就会申请空间不释放，用ExitThread退出会造成内存泄露
	//_beginthreadex和_endthreadex是一对，_endthreadex会先回收资源，再调用ExitThread
	m_handle = (HANDLE)_beginthreadex(nullptr/*线程的安全级别，nullptr代表使用默认级别*/,
									   0/*堆栈大小，0代表使用默认堆栈大小1M*/,
									   &recvThread/*线程函数的起始地址*/,
									   this/*线程函数的参数列表*/,
									   0/*线程初始化标志位，0代表创建即运行*/,
									   nullptr/*操作系统给分配的线程id，输出参数，如果不需要可以不接*/);

	return true;
}

//接收数据的线程函数
unsigned __stdcall UDP::recvThread(void* lpVoid)
{
	UDP* pThis = (UDP*)lpVoid;
	pThis->recvData();
	return 1;
}

//关闭网络(回收线程资源，关闭套接字，卸载库)
void UDP::unInitNet()
{
	//操作系统在创建线程的时候会给每个线程分配3个资源：线程id，句柄，内核对象，引用计数器是2
	// 当引用计数器为0的时候，线程就会被回收
	if (m_handle)
	{
		//1、回收线程资源
	// //1.1 结束线程函数
		m_bRunning = false;
		//等一会，线程走到判断bool值的地方，才能退出while循环
		if (WAIT_TIMEOUT /*等待超时，就是在等待时间到达的时候，线程还没结束*/ ==
			WaitForSingleObject(m_handle, 5000/*等待5000ms*/))
		{
			//强制杀死线程，但是不要一开始强制杀死
			TerminateThread(m_handle/*杀死哪个线程，填的是线程的句柄*/, -1/*退出码*/);
		}
		////1.2、关闭句柄
		CloseHandle(m_handle);
		m_handle = nullptr;
	}

	//2、关闭套接字
	if (m_socket && INVALID_SOCKET != m_socket)
	{
		closesocket(m_socket);
	}

	//3、卸载库
	WSACleanup();
}

//发送数据：阻塞函数
bool UDP::sendData(char* data, int len, u_long to)
{
	//1、判断参数是否合法
	if (!data || len < 0)
	{
		cout << "UDP::sendData error:" << WSAGetLastError() << endl;
		return false;
	}

	//2、发送数据
	sockaddr_in saddrto;
	saddrto.sin_family = AF_INET;
	saddrto.sin_port = htons(UDP_PORT);
	saddrto.sin_addr.S_un.S_addr = to;
	int nsendNum = sendto(m_socket, data, len, 0, (sockaddr*)&saddrto, sizeof(saddrto));
	if (SOCKET_ERROR == nsendNum)
	{
		cout << "UDP::sendData sendto error:" << WSAGetLastError() << endl;
		return false;
	}
	return true;
}

//接收数据：阻塞函数
void UDP::recvData()
{
	int nRecvNum = 0;
	char recvBuf[4096] = "";
	sockaddr_in addrFrom = {};
	int size = sizeof(addrFrom);
	while (m_bRunning)
	{
		nRecvNum = recvfrom(m_socket,recvBuf,sizeof(recvBuf),0,(sockaddr*)&addrFrom, &size);
		if (nRecvNum > 0)
		{
			//接收成功，NEW一个新空间
			char* pack = new char[nRecvNum];
			//把接收到的数据拷贝到新空间中
			memcpy_s(pack,nRecvNum,recvBuf, nRecvNum);
			//把新空间的数据传给中介者类(pack的空间在处理完数据以后再回收)
			m_mediator->transmitData(pack, nRecvNum, addrFrom.sin_addr.S_un.S_addr);
		}
		else
		{
			cout << "UDP::recvData error: " << WSAGetLastError() << endl;
			break;
		}
	}
}



