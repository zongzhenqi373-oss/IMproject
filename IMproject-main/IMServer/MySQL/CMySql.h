#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <mysql.h>
#include <string>
#include <iostream>
#include <mutex>

#pragma comment(lib,"libmysql.lib")

#include <list>
using namespace std;

class CMySql
{
public:
    CMySql(void);
    ~CMySql(void);
public:                    //ip,用户名,密码，数据库，端口号
    bool  ConnectMySql(char *host,char *user,char *pass,char *db,short nport = 3306);
    void  DisConnect();
	// 返回值代表查询语句是否执行成功
    bool  SelectMySql(char* szSql/*查询要执行的sql语句*/,int nColumn/*查询的列的个数*/ ,list<string>& lstStr/*sql语句的查询结果*/);
	// 获得数据库中的表
    bool GetTables(char* szSql,list<string>& lstStr);
    // 更新：删除、插入、修改，返回值代表sql语句是否执行成功
    bool  UpdateMySql(char* szSql);

    // 转义字符串中的特殊字符（' " \ 等），防止 SQL 注入。
    // 用于在 sprintf_s 拼 SQL 前，对来自客户端的用户输入（nick/tel/pass/msg 等）做转义。
    // dst 容量建议至少 2*srcLen+1 字节（最坏情况每个字符都需转义）。
    // 返回转义后字符串长度（不含结尾 \0）；失败返回 -1。
    int   EscapeString(const char* src, int srcLen, char* dst, int dstLen);

 
private:
    MYSQL *m_sock;
	MYSQL_RES *m_results;
	MYSQL_ROW m_record;

	//MySQL C API 单连接非线程安全，且 m_results/m_record 为跨调用共享成员。
	//所有公开接口串行化（recvThread/心跳扫描线程会并发访问本对象）。
	std::mutex m_mutex;

};

