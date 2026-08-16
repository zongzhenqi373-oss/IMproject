#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include<iostream>
#include<Windows.h>
#include<conio.h>
#include"net/UDP.h"
#include"net/TCPClient.h"
#include"net/TCPServer.h"
#include"mediator/UDPmediator.h"
#include"mediator/TCPClientmediator.h"
#include"mediator/TCPServermediator.h"
#include"Kernel.h"

using namespace std;

//优雅关停：控制台终止信号 → 置事件，主线程收到后走 closeServer 正常流程
static HANDLE g_hQuitEvent = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		//已处理，阻止进程被立即终止；主线程负责关停
		if (g_hQuitEvent) SetEvent(g_hQuitEvent);
		return TRUE;
	case CTRL_CLOSE_EVENT:
		//关窗后系统只给进程约 5 秒：置事件后等待主线程完成关停
		if (g_hQuitEvent) SetEvent(g_hQuitEvent);
		Sleep(3000);
		return TRUE;
	default:
		return FALSE;
	}
}

int main()
{
	// Source files, protocol strings and MySQL connections all use UTF-8.
	// Match the Windows console code page so Chinese logs render correctly.
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	Kernel kernel;
	if (!kernel.startServer())
	{
		cout << "打开服务器失败！" << endl;
		return 1;
	}

	//优雅关停：q 键 / Ctrl+C / Ctrl+Break / 关闭窗口
	g_hQuitEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

	cout << "Server is running! 按 q 或 Ctrl+C 优雅关停服务器..." << endl;
	while (WaitForSingleObject(g_hQuitEvent, 500) == WAIT_TIMEOUT)
	{
		if (_kbhit())
		{
			const int ch = _getch();
			if (ch == 'q' || ch == 'Q') break;
		}
	}

	cout << "正在关停服务器..." << endl;
	kernel.closeServer();   //停心跳扫描线程 → 关网络（join 全部工作线程）→ 断开数据库
	CloseHandle(g_hQuitEvent);
	g_hQuitEvent = nullptr;
	cout << "服务器已关停。" << endl;
	return 0;
}

//IM聊天系统（C/S架构）
//注册
//登录
//聊天
//添加好友
//下线

//服务端（VS）
//数据库类（连接数据库、查询/更新数据库内容、断开连接）
//核心处理类（处理收到的数据、组织要发送的数据）
//中介者类（发送数据、把接收到的数据传给核心处理类、打开网络、关闭网络）
//网络类（发送数据、接收数据、初始化网络、关闭网络）---//封装、继承和多态



//客户端（QT）
//UI界面类
//核心处理类（处理收到的数据、组织要发送的数据）
//中介者类（发送数据、把接收到的数据传给核心处理类、打开网络、关闭网络）
//网络类（发送数据、接收数据、初始化网络、关闭网络）


//网络类（）
//父类
//TCP客户端类
//TCP服务端类
//UDP类

//变量类型：
//全局变量
//局部变量
//类成员变量
//静态类成员变量

// 编译错误类型:
// 1、无法实例化抽象类:父类的纯虚函数子类没实现
// 2、已有主体:函数实现了多次
// 3、无法打开源文件:不知道文件所在路径，需要再项目属性中配置

//STL特点
//数组：空间连续，知道下标情况下查找快，增加和删除麻烦
//链表：空间不连续，查找慢，增加和删除容易
//栈：先进后出
//队列：先进先出
//map：保存一对一关系，键值对，查找快
//set：key=value，，查找快

//数据库
//用户信息表：t_user(id主键自增，昵称 唯一非空，电话号码 唯一非空，密码 非空，签名 头像id)
//好友关系表：t_friend(idA、idB)——双向存储，采用空间换时间的方式

// 存在的问题:
//1、同一个用户可以同时多次登录，但只有最后一个用户能收到消息
//2、用户异常下线，服务端不知道用户下线了一-心跳机制
//3、服务器采用I0阻塞多线程模型，部署在windows下，windows给每个进程分配4G的虚拟内存
//0-2G是用户空间，2-4G是内核空间，一个线程的堆栈是1M，
// 2G的用户空间最多可以创建2千多个线程允许2千多个用户同时在线——服务器部署在1inux下使用epo11线程池模型

//扩展
//1、保存聊天记录：本地保存（定期删除）和服务器保存（支持选择日期下载——数据库保存）
//2、发送文件：数据库保存文件路径，定期删除，是否限制文件大小
//3、音视频通话：——音视频编解码技术
//4、推荐好友：给用户加标签，推荐相同标签的用户为好友
