// im_cli：client_core 的命令行演示客户端
// 用途：Mac 本机不依赖 Qt/Android，直接连 im_server 真实互聊（演示/联调/兜底第二端）
//
// 用法: im_cli [ip] [port] [tls_server_name] [ca_file]        默认 127.0.0.1 24563 im.example.com <im_server 开发证书>
// tls_server_name/ca_file 用于连接非本机部署的真实服务器时，匹配对方证书的实际域名/受信任 CA
// 命令:
//   register <昵称> <手机号> <密码>     注册
//   login <手机号> <密码>               登录（种子用户: 张三 13800000001 / 李四 13800000002 / 王五 13800000003，密码均 123456）
//   send <好友id> <内容>                发文本
//   sendimg <好友id> <图片路径>          发图片
//   quit                              下线退出
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "client_core/ClientCore.h"
#include "client_core/Protocol.h"
#include "image_format.h"

namespace {

class CliEvents : public im::IClientEvents {
public:
    void onRegisterResult(int result) override
    {
        std::cout << "\n[注册结果] " << (result == im::proto::REGISTER_SUCC ? "成功"
                  : result == im::proto::REGISTER_NICK_EXIT ? "失败：昵称已存在"
                  : "失败：手机号已存在") << std::endl;
    }
    void onLoginResult(int result, int userId) override
    {
        if (result == im::proto::LOGIN_SUCCESS)
            std::cout << "\n[登录成功] 我的 id=" << userId << std::endl;
        else
            std::cout << "\n[登录失败] " << (result == im::proto::LOGIN_NOTEXIT ? "用户不存在" : "密码错误") << std::endl;
    }
    void onSelfInfo(const im::UserInfo& info) override
    {
        std::cout << "[我的资料] " << info.nick << "（" << info.feeling << "）" << std::endl;
    }
    void onFriendInfo(const im::FriendInfo& info) override
    {
        std::cout << "[好友] id=" << info.id << " " << info.nick
                  << (info.status == im::proto::STATUS_ONLINE ? "（在线）" : "（离线）") << std::endl;
    }
    void onChatMessage(int fromId, const std::string& msg) override
    {
        std::cout << "\n[收到消息] 来自 " << fromId << ": " << msg << std::endl;
    }
    void onImageMessage(int fromId, const std::string& bytes, int w, int h, const std::string&) override
    {
        const std::string path = "/tmp/im_cli_recv_" + std::to_string(fromId) + im::imageExtForBytes(bytes);
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout << "\n[收到图片] 来自 " << fromId << " " << w << "x" << h
                  << " 已存到 " << path << std::endl;
    }
    void onChatSendResult(int friId, int result) override
    {
        std::cout << "[发送回执] 给 " << friId << " "
                  << (result == im::proto::CHAT_RESULT_SUCC ? "已送达" : "对方离线，已转存") << std::endl;
    }
    void onAddFriendRequest(int fromId, const std::string& fromNick) override
    {
        std::cout << "\n[好友申请] " << fromNick << "(id=" << fromId << ") 请求加你" << std::endl;
    }
    void onAddFriendResult(int result, const std::string& destNick) override
    {
        std::cout << "\n[好友申请结果] " << destNick << " result=" << result << std::endl;
    }
    void onFriendOffline(int userId) override
    {
        std::cout << "[好友下线] id=" << userId << std::endl;
    }
    void onKickedOffline(int) override
    {
        std::cout << "\n[被踢下线] 你的账号已在其他设备登录" << std::endl;
    }
    void onConnectionClosed() override
    {
        std::cout << "[连接已断开]" << std::endl;
    }
};

void printHelp()
{
    std::cout << "命令: register <昵称> <手机号> <密码> | login <手机号> <密码> | "
                 "send <好友id> <内容> | sendimg <好友id> <图片路径> | quit" << std::endl;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string ip = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : im::proto::TCP_PORT;
    // tls_server_name/ca_file 都可以在命令行覆盖：连本机开发服务器时用默认值即可；
    // 连真实部署的服务器时，需要传对方证书实际签发的域名 + 信任的 CA，不能一直用开发证书糊弄过去
    const std::string tlsServerName = argc > 3 ? argv[3] : "im.example.com";
    const std::string caFile = argc > 4 ? argv[4] : IM_CLI_DEFAULT_CA;

    im::ClientCore core({tlsServerName, caFile});
    CliEvents events;
    core.setEventSink(&events);

    if (!core.connectToServer(ip, port)) {
        std::cerr << "连接服务端失败 " << ip << ":" << port << std::endl;
        return 1;
    }
    std::cout << "已连接 " << ip << ":" << port << std::endl;
    printHelp();

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "login") {
            std::string tel, pass;
            iss >> tel >> pass;
            core.sendLogin(tel, pass);
        } else if (cmd == "register") {
            std::string nick, tel, pass;
            iss >> nick >> tel >> pass;
            core.sendRegister(nick, tel, pass);
        } else if (cmd == "send") {
            int friId = 0;
            iss >> friId;
            std::string msg;
            std::getline(iss, msg);
            if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            if (friId > 0 && !msg.empty()) core.sendChatMessage(friId, msg);
        } else if (cmd == "sendimg") {
            int friId = 0;
            std::string path;
            iss >> friId >> path;
            std::ifstream ifs(path, std::ios::binary | std::ios::ate);
            if (!ifs) { std::cout << "打不开文件 " << path << std::endl; continue; }
            const auto size = ifs.tellg();
            std::string bytes(static_cast<std::size_t>(size), '\0');
            ifs.seekg(0);
            ifs.read(bytes.data(), size);
            core.sendImageMessage(friId, bytes, 0, 0);
            std::cout << "[发送图片] " << path << " (" << size << " 字节)" << std::endl;
        } else if (cmd == "quit" || cmd == "exit") {
            core.sendOfflineNotify();
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // 等下线包发出
            break;
        } else if (!cmd.empty()) {
            printHelp();
        }
    }

    core.disconnect();
    return 0;
}
