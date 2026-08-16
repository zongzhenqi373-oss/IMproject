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
	err = ::bind(m_socket, (sockaddr*)&saddr, sizeof(saddr));
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
			cout << "Client IP: " << inet_ntoa(addrClient.sin_addr) << endl;
			//给链接成功的client创建一个接收数据的线程（socket 随参数直传）
			RecvCtx* ctx = new RecvCtx{ pThis, sock_accept };
			handle = (HANDLE)_beginthreadex(nullptr, 0, &recvThread, ctx, 0, &threadid);
			if (handle)
			{
				{
					lock_guard<mutex> lock(pThis->m_addrFromMutex);
					pThis->m_listHandle.push_back(handle);
					pThis->m_addrFrom[threadid] = sock_accept;
				}
			}
			else
			{
				//线程创建失败：连接无人接收，直接关闭并释放参数，防止泄漏
				delete ctx;
				closesocket(sock_accept);
			}
		}
	}
	return 1;
}

//接收数据的线程函数
unsigned __stdcall TCPServer::recvThread(void* lpVoid)
{
	RecvCtx* ctx = (RecvCtx*)lpVoid;
	TCPServer* pThis = ctx->self;
	SOCKET s = ctx->sock;
	delete ctx;
	pThis->recvData(s);
	return 1;
}

//关闭指定客户端连接（业务层经 mediator 调用）
void TCPServer::closeConnection(NetEndpoint sock)
{
	SOCKET s = INVALID_SOCKET;
	{
		lock_guard<mutex> lock(m_addrFromMutex);
		for (auto it = m_addrFrom.begin(); it != m_addrFrom.end(); ++it)
		{
			if (it->second == (SOCKET)sock)
			{
				s = it->second;
				m_addrFrom.erase(it);
				break;
			}
		}
	}
	//只有条目存在（连接存活且归本层管理）时才执行关闭；否则说明已被本层关闭，直接返回
	if (s == INVALID_SOCKET) return;
	shutdown(s, SD_BOTH);
	closesocket(s);
}

//关闭网络：回收线程资源，关闭套接字，卸载库
void TCPServer::unInitNet()
{
	m_bRunning = false;

	// 先关闭监听 socket，唤醒阻塞中的 accept。
	if (m_socket != INVALID_SOCKET)
	{
		closesocket(m_socket);
		m_socket = INVALID_SOCKET;
	}

	if (m_handle)
	{
		WaitForSingleObject(m_handle, INFINITE);
		CloseHandle(m_handle);
		m_handle = nullptr;
	}

	// 关闭全部客户端 socket，唤醒对应 recv 线程。
	{
		lock_guard<mutex> lock(m_addrFromMutex);
		for (const auto& entry : m_addrFrom)
		{
			if (entry.second != INVALID_SOCKET)
			{
				shutdown(entry.second, SD_BOTH);
				closesocket(entry.second);
			}
		}
		m_addrFrom.clear();
	}

	for (HANDLE handle : m_listHandle)
	{
		if (handle)
		{
			WaitForSingleObject(handle, INFINITE);
			CloseHandle(handle);
		}
	}
	m_listHandle.clear();

	WSACleanup();
}

//发送数据
bool TCPServer::sendData(char* data, int len, NetEndpoint to)
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

//接收数据（socket 由线程参数直传）
void TCPServer::recvData(SOCKET s)
{
	unsigned int threadId = GetCurrentThreadId();

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
					if (nRecvNum == 0)
					{
						cout << "Client disconnected while receiving packet" << endl;
					}
					else
					{
						const int error = WSAGetLastError();
						if (error != WSAENOTSOCK && error != WSA_OPERATION_ABORTED)
						{
							cout << "TCPServer::recvData payload error: " << error << endl;
						}
					}
					delete[] pack;  //异常退出前释放已分配内存
					break;
				}
			}
			if (RecvLen == 0)
			{
				//一个包数据接收完成，把数据传给中介者类，offset是当前接收到的数据长度
				//（DealData 为同步调用，返回后缓冲区即可释放：net 层分配 net 层释放）
				m_mediator->transmitData(pack, offset, s);
				delete[] pack;
				pack = nullptr;
			}
			else
			{
				// RecvLen != 0 说明是异常 break 出来的，pack 已在上面 delete
				pack = nullptr;
			}
		}
		else
		{
			if (nRecvNum == 0)
			{
				cout << "Client disconnected" << endl;
			}
			else
			{
				const int error = WSAGetLastError();
				if (error != WSAENOTSOCK && error != WSA_OPERATION_ABORTED)
				{
					cout << "TCPServer::recvData header error: " << error << endl;
				}
			}
			break;
		}

	}

	//连接收尾（net 层独占关闭权）：
	//仅当自己的条目仍在 m_addrFrom 中时才负责关闭 socket 并上报断开；
	//条目不存在说明 socket 已被 closeConnection/unInitNet 关闭，本线程绝不再碰
	//（句柄值可能已被 Winsock 复用，再 closesocket 会误关新连接）。
	bool owned = false;
	{
		lock_guard<mutex> lock(m_addrFromMutex);
		auto it = m_addrFrom.find(threadId);
		if (it != m_addrFrom.end())
		{
			m_addrFrom.erase(it);
			owned = true;
		}
	}
	if (owned)
	{
		shutdown(s, SD_BOTH);
		closesocket(s);
		//上报业务层：清理 id→socket 映射并广播好友下线
		m_mediator->notifyDisconnect(s);
	}
}
