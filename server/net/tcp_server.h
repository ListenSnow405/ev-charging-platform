#pragma once
// -----------------------------------------------------------------------------
//  server/net/tcp_server.h  —  TCP 服务器：监听 + 组装各组件
//  归属 L1。
//
//  使用说明：只能由 main.cpp 主线调用，listenOn() 后跑 app.exec()，最终停止调 stop()。
//
//  依赖：epoll_loop、thread_pool；持有Qt 对象 QSocketNotifier/QTimer，只能主线使用）
//  线程：所有 Qt 对象、槽函数**仅在 Qt 主线程执行**；不做任何 socket recv/send 系统调用
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
