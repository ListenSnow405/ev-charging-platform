#include "tcp_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn_ctx.h"
#include "logger.h"
#include "session.h"

namespace ecp {

TcpServer::TcpServer(QObject *parent)
    : QObject(parent), m_io(m_pool)
{
    connect(&m_sweepTimer, &QTimer::timeout, this, &TcpServer::onSweep);
}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::listenOn(quint16 port, int poolSize)
{
    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) {
        LOG_E(QStringLiteral("socket() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    int opt = 1;
    // 允许快速重启服务端而不必等 TIME_WAIT 结束
    ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 监听 fd 必须非阻塞：onNewConnection 里循环 accept 到 EAGAIN，
    // 否则会卡在第二次 accept 上，主线程回不到事件循环，QTimer 停摆。
    int lflags = ::fcntl(m_listenFd, F_GETFL, 0);
    ::fcntl(m_listenFd, F_SETFL, lflags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        LOG_E(QStringLiteral("bind(%1) 失败: %2 —— 端口可能被占用")
                  .arg(port).arg(QString::fromLocal8Bit(strerror(errno))));
        ::close(m_listenFd); m_listenFd = -1;
        return false;
    }
    if (::listen(m_listenFd, 64) < 0) {
        LOG_E(QStringLiteral("listen() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        ::close(m_listenFd); m_listenFd = -1;
        return false;
    }

    if (!m_pool.start(poolSize)) { ::close(m_listenFd); m_listenFd = -1; return false; }
    if (!m_io.start())           { m_pool.stop(); ::close(m_listenFd); m_listenFd = -1; return false; }

    m_notifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &TcpServer::onNewConnection);

    m_sweepTimer.start(60000);   // 60s：清理过期会话 + 设备离线判定

    LOG_I(QStringLiteral("服务端已监听 0.0.0.0:%1，线程池 %2").arg(port).arg(poolSize));
    return true;
}

void TcpServer::onNewConnection()
{
    // level-triggered：一次激活可能对应多个排队连接，循环 accept 到 EAGAIN
    for (;;) {
        sockaddr_in peer{};
        socklen_t   len = sizeof(peer);
        const int fd = ::accept(m_listenFd, reinterpret_cast<sockaddr *>(&peer), &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // 已 accept 完
            LOG_W(QStringLiteral("accept() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        LOG_I(QStringLiteral("接受连接 %1:%2 fd=%3")
                  .arg(QString::fromLatin1(ip)).arg(ntohs(peer.sin_port)).arg(fd));
        setupAccepted(fd);
    }
}

void TcpServer::setupAccepted(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);          // epoll 必须非阻塞
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    auto ctx = std::make_shared<ConnectionCtx>(fd);
    m_io.addConnection(std::move(ctx));
}

void TcpServer::onSweep()
{
    SessionTable::instance().sweepExpired();
    // TODO(L1)：设备离线判定 —— last_heartbeat 超时的 t_pile 置 online=0，写 t_pile_log(event=1)
}

void TcpServer::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    if (m_notifier) { m_notifier->setEnabled(false); }
    m_sweepTimer.stop();
    if (m_listenFd >= 0) { ::shutdown(m_listenFd, SHUT_RDWR); ::close(m_listenFd); m_listenFd = -1; }
    m_io.stop();       // 先停 IO 线程（不再投递任务）
    m_pool.stop();     // 再停线程池（排空已投递任务）
    LOG_I(QStringLiteral("服务端已停止"));
}

} // namespace ecp
