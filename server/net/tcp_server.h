#pragma once
// -----------------------------------------------------------------------------
//  server/net/tcp_server.h  —  TCP 服务器
//  归属 L1。 [说明书] 1.6 网络通信由 Socket 编程实现
//
//  用的是 POSIX 原生 socket（socket/bind/listen/accept/recv/send），
//  不是 QTcpServer —— 说明书点名 Socket 编程，这里是考核点。
//
//  模型：主线程 accept，把连接投给 pthread 线程池，每个工作线程服务一条连接
//        到关闭为止。并发连接数上限 = 线程池大小（默认 8，够本项目演示）。
//        [本组自定] 如需更高并发，L1 可后续改为 epoll + 事件驱动。
// -----------------------------------------------------------------------------
#include <QtGlobal>
#include <atomic>
#include "thread_pool.h"

namespace ecp {

class TcpServer
{
public:
    bool listenOn(quint16 port, int poolSize);
    void run();     // 阻塞：accept 循环，直到 stop()
    void stop();

private:
    void serveConnection(int fd);   // 在工作线程中执行

    int                m_listenFd = -1;
    ThreadPool         m_pool;
    std::atomic<bool>  m_running{false};
};

} // namespace ecp
