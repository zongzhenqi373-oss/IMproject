//#include "stdafx.h"
#include "CMySql.h"


CMySql::CMySql(void)
    : m_sock(nullptr), m_results(nullptr), m_record(nullptr)
{
    /*这个函数用来分配或者初始化一个MYSQL对象，用于连接mysql服务端。
    如果你传入的参数是NULL指针，它将自动为你分配一个MYSQL对象，
    如果这个MYSQL对象是它自动分配的，那么在调用mysql_close的时候，会释放这个对象*/
    m_sock = new MYSQL;
    mysql_init(m_sock);
    //全链路统一 UTF-8（utf8mb4 支持 4 字节 emoji，与客户端 QString::toUtf8/fromUtf8 对齐）
    //注意：MySQL 表/列字符集也需为 utf8mb4，见 docs/im_multiplatform_roadmap.md 阶段-1 验证清单
    mysql_set_character_set(m_sock, "utf8mb4");
}


CMySql::~CMySql(void)
{
    if(m_sock) {
		delete m_sock;
		m_sock = NULL;
    }
}

void CMySql::DisConnect()
{
	lock_guard<mutex> lock(m_mutex);
    mysql_close(m_sock);
}

bool CMySql::ConnectMySql(char *host, char *user, char *pass, char *db, short nport)
{
	lock_guard<mutex> lock(m_mutex);
	if (!mysql_real_connect(m_sock, host, user, pass, db, nport, NULL, CLIENT_MULTI_STATEMENTS)) {
		cout << "连接数据库失败，失败错原因：" << mysql_error(m_sock);
        //连接错误
		return false;
	}
    return true;
}
 bool  CMySql::GetTables(char* szSql, list<string>& lstStr)
 {
	lock_guard<mutex> lock(m_mutex);
    if(mysql_query(m_sock, szSql)) {
		return false;
	}

	m_results = mysql_store_result(m_sock);
    if(NULL == m_results) {
		return false;
	}
	while (m_record = mysql_fetch_row(m_results)) {
		lstStr.push_back(m_record[0]);
	}
    return true;
 }
bool CMySql::SelectMySql(char* szSql, int nColumn, list<string>& lstStr)
{
	lock_guard<mutex> lock(m_mutex);
    //mysql_query() 函数用于向 MySQL 发送并执行 SQL 语句
	if(mysql_query(m_sock, szSql)) {
		cout << "查询数据库失败，失败错原因：" << mysql_error(m_sock);
		return false;
	}

     /*·mysql_store_result 对于成功检索了数据的每个查询(SELECT、SHOW、DESCRIBE、EXPLAIN、CHECK TABLE等)
     返回值:
     . CR_COMMANDS_OUT_OF_SYNC 　　以不恰当的顺序执行了命令。
 　　· CR_OUT_OF_MEMORY 　　内存溢出。
 　　· CR_SERVER_GONE_ERROR 　　MySQL服务器不可用。
 　　· CR_SERVER_LOST 　　在查询过程中，与服务器的连接丢失。
 　　· CR_UNKNOWN_ERROR 　　出现未知错误。*/
	m_results = mysql_store_result(m_sock);
    if (NULL == m_results) {
		cout << "查询数据库失败，查询结果为空";
		return false;
	}
	//遍历表中的下一行，取出内容放入m_record 结果集
	while (m_record = mysql_fetch_row(m_results)) {
        
		for(int i = 0; i < nColumn; i++) {
			if(!m_record[i]) {
				lstStr.push_back("");
			} else {
				lstStr.push_back(m_record[i]);
			}
        }
	}
    return true;
}

 bool  CMySql::UpdateMySql(char* szSql)
 {
	lock_guard<mutex> lock(m_mutex);
    if(!szSql) {
		return false;
	}
    if(mysql_query(m_sock, szSql)) {
		cout << "SQL 执行失败！" << endl;
		cout << "错误原因: " << mysql_error(m_sock) << endl;
		cout << "SQL 语句: " << szSql << endl;
		return false;
	}
    return true;
 }

int CMySql::EscapeString(const char* src, int srcLen, char* dst, int dstLen)
{
	lock_guard<mutex> lock(m_mutex);
    if (!m_sock || !src || !dst || srcLen < 0 || dstLen <= 0) {
        return -1;
    }
    // mysql_real_escape_string 最多写入 2*srcLen+1 字节（含 \0）
    if (dstLen < 2 * srcLen + 1) {
        return -1;
    }
    // mysql_real_escape_string 返回转义后的字节数（不含 \0），失败返回 (unsigned long)-1
    unsigned long ret = mysql_real_escape_string(m_sock, dst, src, (unsigned long)srcLen);
    if (ret == (unsigned long)-1) {
        return -1;
    }
    dst[ret] = '\0';
    return (int)ret;
}


