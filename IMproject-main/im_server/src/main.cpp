// im_server 入口
// 用法: im_server [port] [dbPath]
//   默认 port=24563, dbPath=data/im.db（上传目录 uploads/）
#include <cstdlib>
#include <iostream>
#include "Server.h"

int main(int argc, char* argv[])
{
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : 24563;
    const std::string dbPath = argc > 2 ? argv[2] : "data/im.db";

    imsrv::Server server(port, /*ioThreads=*/4, /*dbWorkers=*/2, dbPath, "uploads");
    if (!server.start()) {
        std::cerr << "启动失败" << std::endl;
        return 1;
    }
    server.run();  // 阻塞直到 SIGINT/SIGTERM
    server.stop(); // 在主线程执行关停（join 全部工作线程）
    return 0;
}
