#pragma once
// -----------------------------------------------------------------------------
//  server/net/tcp_server.h  —  TCP 服务器（改造后）
//  归属 L1。
//
//  模型：主线程 Qt 事件循环（QSocketNotifier 监听 listen fd + QTimer 定时清理）
//        + 独立 pthread epoll IO 线程（管理全部业务 fd 的读写）
//        + pthread 线程池（只跑短业务 handler）。
//  socket/bind/listen/accept 仍是 POSIX 调用，仅「何时 accept」交由 Qt 通知。
// -----------------------------------------------------------------------------
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>

#include "thread_pool.h"
#include "epoll_loop.h"

namespace ecp {

class TcpServer : public QObject
{
    Q_OBJECT
public:
    explicit TcpServer(QObject *parent = nullptr);
    ~TcpServer() override;

    bool listenOn(quint16 port, int poolSize);

public slots:
    void stop();

private slots:
    void onNewConnection();   // QSocketNotifier 触发：accept
    void onSweep();           // QTimer 触发：清理过期会话 + 设备离线判定

private:
    void setupAccepted(int fd);

    int              m_listenFd = -1;
    ThreadPool       m_pool;
    EpollLoop        m_io;                 // 依赖 m_pool，声明在其后
    QSocketNotifier *m_notifier = nullptr;
    QTimer           m_sweepTimer;
    bool             m_stopped = false;    // stop() 幂等保护
};

} // namespace ecp
