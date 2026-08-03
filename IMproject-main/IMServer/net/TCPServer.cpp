#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include"TCPServer.h"
#include"../mediator/TCPServermediator.h"

TCPServer::TCPServer(INetmediator* p)
{
	m_mediator = p;
}
TCPServer::~TCPServer()
{

}

//初始化网络:加载库，创建套接字TCP，绑定，监听，创建接收连接的线程，接收连接（循环）
bool TCPServer::initNet()
{
	//1、加载库
	WORD version = MAKEWORD(2, 2);
	WSADATA data = {  };
	int err = WSAStartup(version, &data);
	if (err != 0)
	{
		cout << "TCPServer::WSAStartup fail:" << err << endl;
		return false;
	}

	//加载库成功，判断库的版本号是否正确
	if (HIBYTE(data.wVersion) == 2 && LOBYTE(data.wVersion) == 2)
	{
		cout << "TCPServer::WSAStartup success!" << endl;
	}
	else   //虽然加载库成功了，但是版本号不正确
	{
		cout << "TCPServer::WSAStartup error!" << endl;

		return false;
	}

	//2、创建套接字
	m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_socket == INVALID_SOCKET)
	{
		cout << "TCPServer::socket error:" << WSAGetLastError() << endl;
		return false;
	}
	else
	{
		cout << "TCPServer::socket success!" << endl;
	}

	//3、绑定
	sockaddr_in saddr;
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(TCP_PORT);
	saddr.sin_addr.S_un.S_addr = ADDR_ANY;
	err = bind(m_socket, (sockaddr*)&saddr, sizeof(saddr));
	if (err == SOCKET_ERROR)
	{
		cout << "TCPServer::bind error:" << WSAGetLastError()/*打印错误码*/ << endl;
		return false;
	}
	else
	{
		cout << "TCPServer::bind success!" << endl;
	}
	//监听
	err = listen(m_socket, TCP_LISTEN_QUEUE_LEN);
	if (err == SOCKET_ERROR)
	{
		cout << "TCPServer::listen error:" << WSAGetLastError()/*打印错误码*/ << endl;
		return false;
	}
	//创建接收连接的线程
	m_handle = (HANDLE)_beginthreadex(nullptr, 0, &acceptThread, this, 0, nullptr);

	return true;
}

//接收连接的线程函数（循环接收连接）
unsigned __stdcall TCPServer::acceptThread(void* lpVoid)
{
	sockaddr_in addrClient = {};
	int addrsize = sizeof(addrClient);
	SOCKET sock_accept = INVALID_SOCKET;
	TCPServer* pThis = (TCPServer*)lpVoid;
	HANDLE handle = nullptr;
	unsigned int threadid = 0;
	while (pThis->m_bRunning)
	{
		sock_accept = accept(pThis->m_socket, (sockaddr*)&addrClient, &addrsize);
		if (sock_accept == INVALID_SOCKET)
		{
			cout << "TCPServer::accept error:" << WSAGetLastError()/*打印错误码*/ << endl;
		}
		else
		{
			//连接成功
			cout << "clien ip：" << inet_ntoa(addrClient.sin_addr) << endl;
			//给链接成功的client创建一个接收数据的线程
			handle = (HANDLE)_beginthreadex(nullptr, 0, &recvThread, pThis, 0, &threadid);
			if (handle)
			{
				{
					lock_guard<mutex> lock(pThis->m_addrFromMutex);
					pThis->m_listHandle.push_back(handle);
					pThis->m_addrFrom[threadid] = sock_accept;
				}
			}
		}
	}
	return 1;
}

//接收数据的线程函数
unsigned __stdcall TCPServer::recvThread(void* lpVoid)
{
	TCPServer* pThis = (TCPServer*)lpVoid;
	pThis->recvData();
	return 1;
}

//关闭网络：回收线程资源，关闭套接字，卸载库
void TCPServer::unInitNet()
{
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

	//句柄链表
	HANDLE h = nullptr;
	for (auto ite = m_listHandle.begin(); ite != m_listHandle.end();)
	{
		h = *ite;
		if (h)
		{
			//1、回收线程资源
		// //1.1 结束线程函数
			m_bRunning = false;
			//等一会，线程走到判断bool值的地方，才能退出while循环
			if (WAIT_TIMEOUT /*等待超时，就是在等待时间到达的时候，线程还没结束*/ ==
				WaitForSingleObject(h, 5000/*等待5000ms*/))
			{
				//强制杀死线程，但是不要一开始强制杀死
				TerminateThread(h/*杀死哪个线程，填的是线程的句柄*/, -1/*退出码*/);
			}
			////1.2、关闭句柄
			CloseHandle(h);
			m_handle = nullptr;
		}
		ite = m_listHandle.erase(ite);
	}

	//2、关闭套接字
	if (m_socket && INVALID_SOCKET != m_socket)
	{
		closesocket(m_socket);
	}

	//图中关闭套接字（加锁后遍历关闭并清空）
	{
		lock_guard<mutex> lock(m_addrFromMutex);
		SOCKET ss = INVALID_SOCKET;
		for (auto ite = m_addrFrom.begin(); ite != m_addrFrom.end();)
		{
			ss = ite->second;
			if (ss && INVALID_SOCKET != ss)
			{
				closesocket(ss);
			}
			ite = m_addrFrom.erase(ite);
		}
	}

	//3、卸载库
	WSACleanup();
}

//发送数据
bool TCPServer::sendData(char* data, int len, u_long to)
{
	//注意socket应该填什么
	// 1、校验参数合法性
	if (!data || len < 1)
	{
		cout << "TCPServer::sendData paramater error" << endl;
		return false;
	}

	// 1.1 长度上限保护（防止业务层误传超大包）
	if (len > MAX_PACK_LEN)
	{
		cout << "TCPServer::sendData len over MAX_PACK_LEN:" << len << endl;
		return false;
	}

	SOCKET sockTo = (SOCKET)to;

	// 2、先发包长度len（统一转网络字节序大端，跨平台一致）
	int netLen = htonl(len);
	int nSendNum = send(sockTo, (char*)&netLen, sizeof(netLen), 0);
	if (SOCKET_ERROR == nSendNum)
	{
		cout << "TCPServer::sendData sendto len error:" << WSAGetLastError() << endl;
		return false;
	}
	// 3、再发包数据data
	nSendNum = send(sockTo, data, len, 0);
	if (SOCKET_ERROR == nSendNum)
	{
		cout << "TCPServer::sendData sendto data error:" << WSAGetLastError() << endl;
		return false;
	}

	return true;
}

//接收数据
void TCPServer::recvData()
{
	//休眠一会：为了保证acceptTheard能够运行到向map中保存threadid的那行代码
	Sleep(5);

	//取出当前线程对应的socket
	//获取当前线程的id
	unsigned int threadId = GetCurrentThreadId();
	SOCKET s = INVALID_SOCKET;
	//从map中取出id对应的socket（加锁，用 find 一次性获取，避免 count+[] 的 TOCTOU）
	{
		lock_guard<mutex> lock(m_addrFromMutex);
		auto it = m_addrFrom.find(threadId);
		if (it != m_addrFrom.end())
		{
			s = it->second;
		}
	}
	if (s == INVALID_SOCKET)
	{
		//说明：acceptThread线程可能没创建
		cout << "TCPServer::recvData socket error" << endl;
		return;
	}

	//先接受数据长度，在接收数据内容
	int offset = 0; //
	int nRecvNum = 0;
	int RecvLen = 0;
	while (m_bRunning) {
		offset = 0;  //清空包长度

		// 1、先收包长度（4字节）
		nRecvNum = recv(s, (char*)&RecvLen, sizeof(RecvLen), 0);
		if (nRecvNum > 0)
		{
			// 1.1 网络字节序转主机序
			RecvLen = ntohl(RecvLen);

			// 1.2 长度上限保护（防止异常/恶意超大 RecvLen 导致 OOM/DoS）
			if (RecvLen <= 0 || RecvLen > MAX_PACK_LEN)
			{
				cout << "TCPServer::recvData illegal RecvLen:" << RecvLen << endl;
				break;
			}

			// 2、接收包长度成功，再接收包数据
			char* pack = new char[RecvLen];
			//开始接收一个包的数据
			while (RecvLen > 0)
			{
				nRecvNum = recv(s, pack + offset, RecvLen, 0);
				if (nRecvNum > 0)
				{
					// 接收一个小包数据成功
					offset += nRecvNum;    //记录累计接收到的数据
					RecvLen -= nRecvNum;   //记录空间剩余大小
				}
				else
				{
					cout << "TCPServer::recvData Data error:" << WSAGetLastError() << endl;
					delete[] pack;  //异常退出前释放已分配内存
					break;
				}
			}
			if (RecvLen == 0)
			{
				//一个包数据接收完成，把数据传给中介者类，offset是当前接收到的数据长度
				m_mediator->transmitData(pack, offset, s);
			}
			else
			{
				// RecvLen != 0 说明是异常 break 出来的，pack 已在上面 delete
				pack = nullptr;
			}
		}
		else
		{
			cout << "TCPServer::recvData Len error:" << WSAGetLastError() << endl;
			break;
		}

	}
}