#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include"TCPClient.h"
#include"../mediator/TCPClientmediator.h"

TCPClient::TCPClient(INetmediator* p)
{
	m_mediator = p;
}
TCPClient::~TCPClient()
{

}

//初始化网络
bool TCPClient::initNet()
{
	//注意：1、不用回收资源，失败直接return false；2、创建的套接字赋值给成员变量
	//1、加载库
	WORD version = MAKEWORD(2, 2);
	WSADATA data = {  };
	int err = WSAStartup(version, &data);
	if (err != 0)
	{
		cout << "TCPClient::WSAStartup fail:" << err << endl;
		return false;
	}

	//加载库成功，判断库的版本号是否正确
	if (HIBYTE(data.wVersion) == 2 && LOBYTE(data.wVersion) == 2)
	{
		cout << "TCPClient::WSAStartup success!" << endl;
	}
	else   //虽然加载库成功了，但是版本号不正确
	{
		cout << "TCPClient::WSAStartup error!" << endl;

		return false;
	}

	//2、创建套接字
	m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_socket == INVALID_SOCKET)
	{
		cout << "TCPClient::socket error:" << WSAGetLastError() << endl;
		return false;
	}
	else
	{
		cout << "TCPClient::socket success!" << endl;
	}

	//申请连接
	sockaddr_in saddr;
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(TCP_PORT);
	saddr.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
	err = connect(m_socket, (sockaddr*)&saddr, sizeof(saddr));
	if (err == SOCKET_ERROR)
	{
		cout << "TCPClient::connect error:" << WSAGetLastError()/*打印错误码*/ << endl;
		return false;
	}
	else
	{
		cout << "TCPClient::connect success!" << endl;
	}

	//4、创建接收数据的线程
	m_handle = (HANDLE)_beginthreadex(nullptr, 0, &recvThread, this, 0, nullptr);

	return true;
}

//接收数据的线程函数
unsigned __stdcall TCPClient::recvThread(void* lpVoid)
{
		TCPClient* pThis = (TCPClient*)lpVoid;
		pThis->recvData();
		return 1;
}


//关闭网络：回收线程资源，关闭套接字，卸载库
void TCPClient::unInitNet()
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

//发送数据
bool TCPClient::sendData(char* data, int len, u_long to)
{
	// 1、校验参数合法性
	if (!data || len < 1) 
	{
		cout << "TcpClient::sendData paramater error" << endl;
		return false;
	}

	// 2、先发包长度len
	int nSendNum = send(m_socket, (char*)&len, sizeof(len), 0);
	if (SOCKET_ERROR == nSendNum)
	{
		cout << "TcpClient::sendData sendto len error:" << WSAGetLastError() << endl;
		return false;
	}
	// 3、再发包数据data
	nSendNum = send(m_socket, data, len, 0);
	if (SOCKET_ERROR == nSendNum)
	{
		cout << "TcpClient::sendData sendto data error:" << WSAGetLastError() << endl;
		return false;
	}

	return true;
}

//接收数据
void TCPClient::recvData()
{
	int offset = 0; //
	int nRecvNum = 0;
	int RecvLen = 0;
	while (m_bRunning) {
		offset = 0;  //清空包长度

		// 1、先收包长度
		nRecvNum = recv(m_socket, (char*)&RecvLen, sizeof(RecvLen),0);
		if (nRecvNum > 0)
		{
			// 2、接收包长度成功，再接收包数据
			char* pack = new char[RecvLen];
			//开始接收一个包的数据
			while (RecvLen > 0)
			{
				nRecvNum = recv(m_socket, pack + offset, RecvLen, 0);
				if (nRecvNum > 0)
				{
					// 接收一个小包数据成功
					offset += nRecvNum;    //记录累计接收到的数据
					RecvLen -= nRecvNum;   //记录空间剩余大小
				}
				else
				{
					cout << "TCPClient::recvData Data error:" << WSAGetLastError() << endl;
					break;
				}
			}
			//一个包数据接收完成，把数据传给中介者类，offset是当前接收到的数据长度
			m_mediator->transmitData(pack, offset, m_socket);
		}
		else 
		{
			cout << "TCPClient::recvData Len error:" << WSAGetLastError() << endl;
			break;
		}

	}
}