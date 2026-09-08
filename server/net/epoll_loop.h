#pragma once
// -----------------------------------------------------------------------------
//  server/net/epoll_loop.h  —  epoll IO 线程
//  归属 L1。pthread 线程，统一管理全部业务 socket 的读写。
//   - recv + FrameParser 切帧（粘包/半包），不执行业务；
//   - 切出完整 payload 后投递短任务到线程池；
//   - EPOLLOUT 时从各连接发送队列取帧 send。
// -----------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <pthread.h>

#include <QHash>

#include "conn_ctx.h"

namespace ecp {

class ThreadPool;

class EpollLoop
{
public:
    explicit EpollLoop(ThreadPool &pool);
    ~EpollLoop();

    bool start();
    void stop();

    // 主线程调用：接受连接后把 ctx 加入 epoll（只监听 EPOLLIN）。
    void addConnection(std::shared_ptr<ConnectionCtx> ctx);

private:
    static void *entry(void *arg);
    void run();
    void handleEvent(int fd, uint32_t events);
    void handleReadable(const std::shared_ptr<ConnectionCtx> &ctx);
    void handleWritable(const std::shared_ptr<ConnectionCtx> &ctx);
    void closeConnection(const std::shared_ptr<ConnectionCtx> &ctx);
    void modEvent(int fd, uint32_t events);
    void drainWakeup();

    ThreadPool &m_pool;
    int         m_epollFd = -1;
    int         m_wakeFd  = -1;
    std::atomic<bool> m_running{false};
    pthread_t   m_thread{};

    std::mutex  m_mtx;                              // 保护 m_conns
    QHash<int, std::shared_ptr<ConnectionCtx>> m_conns;
};

} // namespace ecp
