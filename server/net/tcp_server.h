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
//
//  退出：信号处理函数不能直接调 stop()（要加锁、要 join，都不是
//        async-signal-safe，且会把主线程锁死在信号上下文里）。这里改成
//        self-pipe：处理函数只往 wakeFd() 写一字节，run() 的 poll 被唤醒后
//        返回，由主线程正常调用 stop() 做清理。
// -----------------------------------------------------------------------------
#include <QtGlobal>
#include <atomic>
#include <pthread.h>
#include <set>
#include "thread_pool.h"

namespace ecp {

class TcpServer
{
public:
    ~TcpServer();

    bool listenOn(quint16 port, int poolSize);
    void run();     // 阻塞：accept 循环，直到被 wakeFd() 唤醒
    void stop();    // 在主线程调用，不要在信号处理函数里调

    // self-pipe 的写端：信号处理函数往这里写 1 字节即可唤醒 run()。
    // write() 是 async-signal-safe 的，处理函数里只允许做这件事。
    int wakeFd() const { return m_wakeFd[1]; }

private:
    void serveConnection(int fd);   // 在工作线程中执行

    // 活动连接登记表：stop() 要靠它 shutdown 掉阻塞在 recv 上的连接，
    // 否则工作线程永远回不来，pthread_join 就会一直卡住。
    bool addConn(int fd);           // 已停止则返回 false
    void removeConn(int fd);

    int                m_listenFd   = -1;
    int                m_wakeFd[2]  = {-1, -1};   // [0] 读端给 poll，[1] 写端给信号处理函数
    ThreadPool         m_pool;
    std::atomic<bool>  m_running{false};

    std::set<int>      m_conns;
    pthread_mutex_t    m_connMtx = PTHREAD_MUTEX_INITIALIZER;
};

} // namespace ecp
