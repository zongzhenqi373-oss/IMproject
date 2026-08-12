#include "Kernel.h"
#include"mediator/TCPServermediator.h"

Kernel* Kernel::m_pKernel = nullptr;
Kernel::Kernel()
{
	setFunArr();
	m_pKernel = this;
	m_pMediator = new TCPServermediator;

}
Kernel::~Kernel()
{
	if (m_pMediator)
	{
		m_pMediator->closeNet();
		delete m_pMediator;
		m_pMediator = nullptr;
	}

}

//初始化函数指针数组
void Kernel::setFunArr()
{
	cout << __func__ << endl;
	//初始化数组
	memset(m_dealFunArr, 0, sizeof(m_dealFunArr));

	//把函数地址保存到数组中
	m_dealFunArr[DEF_PROT_REGISTER_RQ       - DEF_BASE] = &Kernel::DealRegisterRq;
	m_dealFunArr[DEF_PROT_LOGIN_RQ		    - DEF_BASE] = &Kernel::DealLoginRq;
	m_dealFunArr[DEF_PROT_FRIEND_OFFLINE	- DEF_BASE] = &Kernel::DealOfflineRq;
	m_dealFunArr[DEF_PROT_CHAT_INFO_RQ      - DEF_BASE] = &Kernel::DealChatRq;
	m_dealFunArr[DEF_PROT_ADD_FRIEND_RQ		- DEF_BASE] = &Kernel::DealAddFriendRq;
	m_dealFunArr[DEF_PROT_ADD_FRIEND_RS		- DEF_BASE] = &Kernel::DealAddFriendRs;
}

//打开服务器
bool Kernel::startServer()
{
	//打开网络
	if (!m_pMediator->openNet())
	{
		cout << "打开服务器失败！" << endl;
		return false;
	}
	//连接数据库
	char ip[] = "127.0.0.1";
	char user[] = "root";
	char pass[] = "zongzhenqi373";
	char db[] = "20250113im";
	if (!m_mysql.ConnectMySql(ip, user, pass, db))
	{
		cout << "连接数据库失败！" << endl;
		return false;
	}

	return true;
}

//关闭服务器
void Kernel::closeServer()
{
	//关闭网络
	m_pMediator->closeNet();
	//断开与数据库的连接
	m_mysql.DisConnect();

}

//组装并发送一个完整包体：4B 小端协议号 + pb payload（net 层再加 4B 大端包长）
void Kernel::sendPacket(protType type, const std::string& payload, unsigned long to)
{
	std::string body;
	body.resize(sizeof(protType) + payload.size());
	memcpy(&body[0], &type, sizeof(type));           // x86/x64 主机序即小端
	memcpy(&body[sizeof(type)], payload.data(), payload.size());
	m_pMediator->sendData(body.data(), (int)body.size(), to);
}

//处理和分发所有收到的数据
void Kernel::DealData(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	//包体 = [4B 小端协议号][pb payload]
	if (!data || len < (int)sizeof(protType))
	{
		cout << "非法包长度:" << len << endl;
		return;
	}

	//取出协议类型（x86/x64 主机序即小端，与客户端线格式一致）
	protType type = *(protType*)data;

	//计算数组下表
	int index = type - DEF_BASE;

	//判断数组下标是否在有效范围内
	if (index >= 0 && index < DEF_PROT_COUNT)
	{
		//根据数组下标取出函数地址
		DEAL_FUN pFun = m_dealFunArr[index];
		//判断函数指针是否有效
		if (pFun)
		{
			//跳过 4B 协议号，handler 只处理 pb payload
			(this->*pFun)(data + sizeof(protType), len - (int)sizeof(protType), from);
		}
		else  //指针为空：协议号未注册
		{
			cout << "type2:" << type << endl;
		}
	}
	else   //越界：协议号非法
	{
		cout << "type1:" << type << endl;
	}
}

//处理注册请求的函数
void Kernel::DealRegisterRq(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	//1、解析 pb
	im::proto::RegisterRq rq;
	if (!rq.ParseFromArray(data, len))
	{
		cout << "解析注册请求失败！" << endl;
		return;
	}

	//防御性截断（字段软上限，防超长字段写库）
	std::string nick = utf8Truncate(rq.nick(), USER_NICK_LEN - 1);
	std::string tel  = utf8Truncate(rq.tel(),  USER_TEL_LEN - 1);
	std::string pass = utf8Truncate(rq.pass(), USER_PASS_LEN - 1);

	//转义用户输入，防止 SQL 注入（昵称/电话/密码均来自客户端，不可信）
	char escNick[USER_NICK_LEN * 2 + 1] = "";
	char escTel[USER_TEL_LEN * 2 + 1] = "";
	char escPass[USER_PASS_LEN * 2 + 1] = "";
	m_mysql.EscapeString(nick.c_str(), (int)nick.size(), escNick, sizeof(escNick));
	m_mysql.EscapeString(tel.c_str(),  (int)tel.size(),  escTel,  sizeof(escTel));
	m_mysql.EscapeString(pass.c_str(), (int)pass.size(), escPass, sizeof(escPass));

	//2、根据昵称从数据库中查询昵称
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql,"select name from t_user where name = '%s';",escNick);
	if (!m_mysql.SelectMySql(sql,1,listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;
	}

	im::proto::RegisterRs rs;
	//3、判断昵称查询结果是否为空
	if (listRes.size() == 0)
	{
		//如果为空说明昵称未被注册
		//4、根据电话号码从数据库中查询
		sprintf_s(sql, "select tel from t_user where tel = '%s';", escTel);
		if (!m_mysql.SelectMySql(sql, 1, listRes))
		{
			cout << "查询数据库失败！" << sql << endl;
			return;
		}
		//5、判断电话号码查询结果是否为空
		if (listRes.size() == 0)
		{
			//如果为空，说明电话号码没被注册
			//6、把用户注册的信息存入数据库中
			sprintf_s(sql ,"insert into t_user (name,tel,passwd,feeling,iconid) values ('%s','%s','%s','努力实现财富自由',3);"
				      ,escNick,escTel,escPass);
			if (!m_mysql.UpdateMySql(sql))
			{
				cout << "保存信息失败！" << sql << endl;
				return;
			}
			rs.set_result(REGISTER_SUCC);
		}
		else
		{
			//如果不为空，那么说明电话号码被注册过，注册失败
			rs.set_result(REGISTER_TEL_EXIT);
		}
	}
	else
	{
		//如果不为空，说明昵称被注册过，注册失败
		rs.set_result(REGISTER_NICK_EXIT);
	}

	//4、不管注册结果成功还是失败，都要给客户端返回注册结果
	sendPacket(DEF_PROT_REGISTER_RS, rs.SerializeAsString(), from);
}

//处理登录请求的函数
void Kernel::DealLoginRq(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	im::proto::LoginRq rq;
	if (!rq.ParseFromArray(data, len))
	{
		cout << "解析登录请求失败！" << endl;
		return;
	}

	std::string tel = utf8Truncate(rq.tel(), USER_TEL_LEN - 1);

	//转义用户输入的电话号码，防止 SQL 注入
	char escTel[USER_TEL_LEN * 2 + 1] = "";
	m_mysql.EscapeString(tel.c_str(), (int)tel.size(), escTel, sizeof(escTel));

	//根据电话号码查询密码
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql, "select passwd,id from t_user where tel = '%s';", escTel);
	if (!m_mysql.SelectMySql(sql,2,listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;	
	}

	im::proto::LoginRs rs;
	//判断查询密码是否为空
	if (listRes.size() == 0)
	{
		//如果为空，说明电话号码没有注册过，登陆失败
		rs.set_result(LOGIN_NOTEXIT);
		cout << "登录失败！" << endl;
	}
	else
	{
		//如果不为空，那就从listRes中取出密码
		string passLine = listRes.front();
		listRes.pop_front();   //从list中删除取走的数据
		int id = stoi(listRes.front());
		listRes.pop_front();   //从list中删除取走的数据

		//比较取出的密码和登录输入的密码是否相等
		if (passLine == rq.pass())
		{
			//如果相等，那么登录成功
			rs.set_result(LOGIN_SUCCESS);
			rs.set_userid(id);

			//保存当前用户的id和socket（加锁）
			setSocket(id, from);

			sendPacket(DEF_PROT_LOGIN_RS, rs.SerializeAsString(), from);

			//根据id查询当前用户以及好友的信息
			getUserInfoAndFriendInfo(id);

			// 发送离线消息（关键）
			sendOfflinemsg(id);

			return;
		}
		else
		{
			//如果不相等，那么登录失败，密码错误
			rs.set_result(LOGIN_PASSERROR);
		}
	}
	sendPacket(DEF_PROT_LOGIN_RS, rs.SerializeAsString(), from);
}

//根据id查询当前用户以及好友的信息
void Kernel::getUserInfoAndFriendInfo(int id)
{
	cout << __func__ << endl;
	//根据自己的id查询自己的信息
	im::proto::FriendInfo Myinfo;
	getInfoById(id, &Myinfo);

	//把自己的信息发送给客户端（加锁查询 socket）
	unsigned long selfSock = 0;
	if (getSocket(id, selfSock))
	{
		sendPacket(DEF_PROT_FRIEND_INFO, Myinfo.SerializeAsString(), selfSock);
	}
	else
	{
		cout<< "ID:" << id << endl;
	}
	

	//根据自己的id查询好友的id
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql, "select idB from t_friend where idA = '%d';", id);
	if (!m_mysql.SelectMySql(sql, 1, listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;
	}

	//遍历好友列表
	int friendid = 0;
	im::proto::FriendInfo Friendinfo;
	const std::string myInfoPayload = Myinfo.SerializeAsString();
	while (listRes.size() > 0)
	{
		//根据好友的id查询好友的信息
		friendid = stoi(listRes.front());
		listRes.pop_front();   //从list中删除取走的数据
		Friendinfo.Clear();
		getInfoById(friendid, &Friendinfo);

		//把好友的信息发送给客户端（加锁查询 socket）
		if (getSocket(id, selfSock))
		{
			sendPacket(DEF_PROT_FRIEND_INFO, Friendinfo.SerializeAsString(), selfSock);
		}
		else
		{
			cout << "ID:" << id << endl;
		}
		//查看朋友在不在线（加锁）
		unsigned long friSock = 0;
		if (getSocket(friendid, friSock))
		{
			//如果在线，那么给好友发送自己的信息
			sendPacket(DEF_PROT_FRIEND_INFO, myInfoPayload, friSock);
		}
	}
	
}

//根据id查询用户信息
void Kernel::getInfoById(int id, im::proto::FriendInfo* info)
{
	cout << __func__ << endl;
	info->set_userid(id);
	if (isOnline(id))
	{
		//在线
		info->set_status(STATUS_ONLINE);
	}
	else
	{
		//不在线
		info->set_status(STATUS_OFFLINE);
	}
	//根据id查询用户名字，签名，头像id
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql, "select name,feeling,iconid from t_user where id = '%d';", id);
	if (!m_mysql.SelectMySql(sql, 3, listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;
	}

	if (listRes.size() == 3)
	{
		//从list中取出昵称
		string nick = listRes.front();
		info->set_nick(nick);
		listRes.pop_front();   //从list中删除取走的数据

		//从list中取出签名
		string feeling = listRes.front();
		info->set_feeling(feeling);
		listRes.pop_front();   //从list中删除取走的数据

		//从list中取出头像id
		info->set_iconid(stoi(listRes.front()));
		listRes.pop_front();   //从list中删除取走的数据
	}
	else
	{
		cout << "sql；" << sql << endl;
	}
}

//离线消息发送
void Kernel::sendOfflinemsg(int id) {
	cout << __func__ << endl;

	char sql[512] = "";
	sprintf_s(sql,
		"select id,sender_id,content from offline_msg where receiver_id=%d and is_delivered=0 order by send_time ASC;",
		id);

	list<string> lst;
	if (!m_mysql.SelectMySql(sql, 3, lst)) {
		cout << "查询数据库失败！" << sql << endl;
		return;
	}

	if (lst.size()==0) {
		cout << "消息列表没有要发送的数据！" << endl;
	}
	else
	{
		auto it = lst.begin();
		while (it != lst.end())
		{
			int msgId = atoi(it->c_str()); it++;
			int senderId = atoi(it->c_str()); it++;
			string content = *it;
			it++;

			im::proto::ChatInfoRq rq;
			rq.set_myid(senderId);
			rq.set_friid(id);
			rq.set_msg(content);

			// 发送给当前已登录用户（加锁查询 socket）
			unsigned long selfSock = 0;
			if (getSocket(id, selfSock))
			{
				sendPacket(DEF_PROT_CHAT_INFO_RQ, rq.SerializeAsString(), selfSock);
			}


			// 标记该消息已投递
			char updateSql[128] = "";
			sprintf_s(updateSql,
				"update offline_msg set is_delivered=1 where id=%d;",
				msgId);
			m_mysql.UpdateMySql(updateSql);
		}
	}
}

//处理下线请求
void Kernel::DealOfflineRq(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	im::proto::FriendOffline offlineRq;
	if (!offlineRq.ParseFromArray(data, len))
	{
		cout << "解析下线请求失败！" << endl;
		return;
	}
	//1、根据id查找下线用户的好友id列表
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql, "select idB from t_friend where idA = '%d';", offlineRq.offlineid());
	if (!m_mysql.SelectMySql(sql, 1, listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;
	}
	//重组转发用的包体（4B 协议号 + 原 payload，直接透传）
	std::string body;
	body.resize(sizeof(protType) + len);
	protType type = DEF_PROT_FRIEND_OFFLINE;
	memcpy(&body[0], &type, sizeof(type));
	memcpy(&body[sizeof(type)], data, len);

	//2、遍历好友id列表
	int friendid = 0;
	while (listRes.size() > 0)
	{
		//3、取出好友的id
		friendid = stoi(listRes.front());
		//4、从列表中删除已经取出的好友id
		listRes.pop_front();
		//5、判断好友是否在线（加锁查询 socket，锁外转发）
		unsigned long friSock = 0;
		if (getSocket(friendid, friSock))
		{
			//6、如果在线，就给在线好友发送下线请求
			m_pMediator->sendData(body.data(), (int)body.size(), friSock);
		}

	}
	//7、从map中删除下线用户并取出 socket（加锁），锁外 closesocket
	unsigned long offSock = 0;
	if (eraseSocket(offlineRq.offlineid(), offSock))
	{
		if (offSock > 0)
		{
			closesocket((SOCKET)offSock);
		}
	}
}

//处理聊天请求
void Kernel::DealChatRq(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	im::proto::ChatInfoRq rq;
	if (!rq.ParseFromArray(data, len))
	{
		cout << "解析聊天请求失败！" << endl;
		return;
	}
	//判断好友是否在线（加锁查询 socket，锁外转发）
	unsigned long friSock = 0;
	if (getSocket(rq.friid(), friSock))
	{
		//如果在线，那么把聊天请求转发给好友（重组包体透传）
		std::string body;
		body.resize(sizeof(protType) + len);
		protType type = DEF_PROT_CHAT_INFO_RQ;
		memcpy(&body[0], &type, sizeof(type));
		memcpy(&body[sizeof(type)], data, len);
		m_pMediator->sendData(body.data(), (int)body.size(), friSock);
	}
	else
	{
		//如果不在线，那么回复一个不在线状态给客户端并将消息保存到消息列表数据库
		//转义聊天内容，防止 SQL 注入（msg 来自客户端）
		std::string msg = utf8Truncate(rq.msg(), CHAT_MSG_LEN - 1);
		char escMsg[CHAT_MSG_LEN * 2 + 1] = "";
		m_mysql.EscapeString(msg.c_str(), (int)msg.size(), escMsg, sizeof(escMsg));

		char sql[CHAT_MSG_LEN * 2 + 256] = "";
		sprintf_s(sql,
			"insert into offline_msg (sender_id, receiver_id, content) values(%d, %d, '%s');",
			rq.myid(),
			rq.friid(),
			escMsg);

		m_mysql.UpdateMySql(sql);
		cout << "离线消息已保存" << endl;

		im::proto::ChatInfoRs rs;
		rs.set_myid(rq.friid());
		rs.set_friid(0);
		rs.set_result(CHAT_RESULT_FAIL);
		sendPacket(DEF_PROT_CHAT_INFO_RS, rs.SerializeAsString(), from);
	}
}

//处理添加好友请求
void Kernel::DealAddFriendRq(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	im::proto::AddFriendRq rq;
	if (!rq.ParseFromArray(data, len))
	{
		cout << "解析添加好友请求失败！" << endl;
		return;
	}

	//转义好友昵称，防止 SQL 注入（frinick 来自客户端）
	std::string frinick = utf8Truncate(rq.frinick(), USER_NICK_LEN - 1);
	char escFrinick[USER_NICK_LEN * 2 + 1] = "";
	m_mysql.EscapeString(frinick.c_str(), (int)frinick.size(), escFrinick, sizeof(escFrinick));

	//根据好友昵称查询好友id
	list<string> listRes;
	char sql[1024] = "";
	sprintf_s(sql, "select id from t_user where name = '%s';", escFrinick);
	if (!m_mysql.SelectMySql(sql, 1, listRes))
	{
		cout << "查询数据库失败！" << sql << endl;
		return;
	}
	//判断查询结果是否为空
	if (listRes.size() == 0)
	{
		//如果为空，添加好友失败，说明好友不存在，回复一个好友不存在的信息给客户端
		im::proto::AddFriendRs rs;
		rs.set_result(ADD_FRIEND_NOTEXIT);
		rs.set_mynick(rq.frinick());
		sendPacket(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString(), from);
	}
	else
	{
		//如果不为空没，那么用户存在，判断好友当前是否在线（加锁）
		int friendid = 0;
		friendid = stoi(listRes.front());
		listRes.pop_front();
		unsigned long friSock = 0;
		if (getSocket(friendid, friSock))
		{
			//如果在线，将添加好友的请求转发给用户（重组包体透传）
			std::string body;
			body.resize(sizeof(protType) + len);
			protType type = DEF_PROT_ADD_FRIEND_RQ;
			memcpy(&body[0], &type, sizeof(type));
			memcpy(&body[sizeof(type)], data, len);
			m_pMediator->sendData(body.data(), (int)body.size(), friSock);
		}
		else
		{
			//如果不在线，那么回复一个好友不在线的信息给客户端，好友添加失败
			im::proto::AddFriendRs rs;
			rs.set_result(ADD_FRIEND_OFFLINE);
			rs.set_mynick(rq.frinick());
			sendPacket(DEF_PROT_ADD_FRIEND_RS, rs.SerializeAsString(), from);
		}

	}
}

//处理添加好友回复
void Kernel::DealAddFriendRs(char* data, int len, unsigned long from)
{
	cout << __func__ << endl;
	im::proto::AddFriendRs rs;
	if (!rs.ParseFromArray(data, len))
	{
		cout << "解析添加好友回复失败！" << endl;
		return;
	}

	//如果好友同意添加
	if (rs.result() == ADD_FRIEND_AGREE)
	{
		//将双方的好友信息写入到数据库中
		char sql[1024] = "";
		sprintf_s(sql, "insert into t_friend values(%d ,%d) ;", rs.destid() , rs.myid());
		if (!m_mysql.UpdateMySql(sql))
		{
			cout << "插入数据库失败！" << sql << endl;
			return;
		}

		sprintf_s(sql, "insert into t_friend values(%d ,%d) ;", rs.myid() , rs.destid());
		if (!m_mysql.UpdateMySql(sql))
		{
			cout << "插入数据库失败！" << sql << endl;
			return;
		}

		//更新双方好友列表
		getUserInfoAndFriendInfo(rs.destid());

	}
	//无论结果如何，都把回复的数据传给发起好友申请的客户端（重组包体透传）
	unsigned long destSock = 0;
	if (getSocket(rs.destid(), destSock))
	{
		std::string body;
		body.resize(sizeof(protType) + len);
		protType type = DEF_PROT_ADD_FRIEND_RS;
		memcpy(&body[0], &type, sizeof(type));
		memcpy(&body[sizeof(type)], data, len);
		m_pMediator->sendData(body.data(), (int)body.size(), destSock);
	}

}

//==================== m_mapIdtoSocket 加锁辅助方法 ====================
//注意：所有 m_mapIdtoSocket 的读写必须通过这些方法，禁止直接访问。
//锁内只做 map 查询/修改，锁外调用 sendData，避免持锁阻塞导致死锁/降并发。

bool Kernel::getSocket(int id, unsigned long& out)
{
	lock_guard<mutex> lock(m_mapIdtoSocketMutex);
	auto it = m_mapIdtoSocket.find(id);
	if (it != m_mapIdtoSocket.end())
	{
		out = it->second;
		return true;
	}
	return false;
}

void Kernel::setSocket(int id, unsigned long sock)
{
	lock_guard<mutex> lock(m_mapIdtoSocketMutex);
	m_mapIdtoSocket[id] = sock;
}

bool Kernel::isOnline(int id)
{
	lock_guard<mutex> lock(m_mapIdtoSocketMutex);
	return m_mapIdtoSocket.count(id) > 0;
}

bool Kernel::eraseSocket(int id, unsigned long& outSock)
{
	lock_guard<mutex> lock(m_mapIdtoSocketMutex);
	auto it = m_mapIdtoSocket.find(id);
	if (it != m_mapIdtoSocket.end())
	{
		outSock = it->second;
		m_mapIdtoSocket.erase(it);
		return true;
	}
	return false;
}
