#include "tcp_server.h"
#include "dispatcher.h"
#include "frame.h"
#include "logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ecp {

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

    // 停止用的 self-pipe，必须在起线程池之前建好
    if (::pipe(m_wakeFd) < 0) {
        LOG_E(QStringLiteral("pipe() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        ::close(m_listenFd); m_listenFd = -1;
        return false;
    }
    // 两端都设非阻塞：写端是因为信号处理函数里的 write 绝不能阻塞；
    // 读端是因为 run() 里排空管道时，最后一次 read 无数据会一直挂住。
    ::fcntl(m_wakeFd[0], F_SETFL, ::fcntl(m_wakeFd[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(m_wakeFd[1], F_SETFL, ::fcntl(m_wakeFd[1], F_GETFL, 0) | O_NONBLOCK);

    if (!m_pool.start(poolSize)) {
        ::close(m_wakeFd[0]); ::close(m_wakeFd[1]); m_wakeFd[0] = m_wakeFd[1] = -1;
        ::close(m_listenFd); m_listenFd = -1;
        return false;
    }

    m_running = true;
    LOG_I(QStringLiteral("服务端已监听 0.0.0.0:%1，线程池 %2").arg(port).arg(poolSize));
    return true;
}

void TcpServer::run()
{
    while (m_running) {
        // 同时等监听 fd 和 self-pipe：后者可读说明收到了停止信号
        struct pollfd fds[2];
        fds[0].fd = m_listenFd;  fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = m_wakeFd[0]; fds[1].events = POLLIN; fds[1].revents = 0;

        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;        // 被信号打断，正常
            if (!m_running) break;
            LOG_W(QStringLiteral("poll() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            continue;
        }

        if (fds[1].revents & POLLIN) {           // 收到停止通知 → 退出 accept 循环
            char drain[64];
            while (::read(m_wakeFd[0], drain, sizeof(drain)) > 0) { }
            break;
        }
        if (!(fds[0].revents & POLLIN)) continue;

        sockaddr_in peer{};
        socklen_t   len = sizeof(peer);
        const int fd = ::accept(m_listenFd, reinterpret_cast<sockaddr *>(&peer), &len);
        if (fd < 0) {
            if (errno == EINTR) continue;        // 被信号打断，正常
            if (!m_running) break;               // stop() 关闭了监听 fd
            LOG_W(QStringLiteral("accept() 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        LOG_I(QStringLiteral("接受连接 %1:%2 fd=%3")
                  .arg(QString::fromLatin1(ip)).arg(ntohs(peer.sin_port)).arg(fd));

        if (!m_pool.post([this, fd] { serveConnection(fd); })) {
            LOG_W(QStringLiteral("线程池已停止，拒绝连接 fd=%1").arg(fd));
            ::close(fd);
        }
    }
}

bool TcpServer::addConn(int fd)
{
    pthread_mutex_lock(&m_connMtx);
    // m_running 在 stop() 里先于取锁被置 false：若这里读到 true，
    // 说明本次登记一定排在 stop() 遍历之前，不会漏掉这条连接。
    const bool ok = m_running.load();
    if (ok) m_conns.insert(fd);
    pthread_mutex_unlock(&m_connMtx);
    return ok;
}

void TcpServer::removeConn(int fd)
{
    pthread_mutex_lock(&m_connMtx);
    m_conns.erase(fd);
    pthread_mutex_unlock(&m_connMtx);
}

void TcpServer::serveConnection(int fd)
{
    if (!addConn(fd)) {          // 已经在停止了，直接谢客
        ::close(fd);
        return;
    }

    FrameParser parser;              // ⚠ 每条连接一个解析器，负责处理粘包/半包
    char buf[4096];

    while (m_running) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) { LOG_I(QStringLiteral("对端关闭 fd=%1").arg(fd)); break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!m_running) break;   // stop() 半关闭了本连接，属正常退出
            LOG_W(QStringLiteral("recv 失败 fd=%1: %2").arg(fd).arg(QString::fromLocal8Bit(strerror(errno))));
            break;
        }

        // ⚠ CLAUDE.md 硬性规则第 1 条：
        //   把收到的字节喂给 FrameParser，由它切出完整报文。
        //   绝不能把这一次 recv 的结果直接当成一个完整包去解析。
        parser.append(QByteArray(buf, static_cast<int>(n)));

        QByteArray payload;
        while (parser.next(payload)) {
            const QByteArray resp  = Dispatcher::instance().handle(payload);
            const QByteArray frame = encodeFrame(resp);

            // 写也要循环，send 可能只写出一部分
            qint64 sent = 0;
            while (sent < frame.size()) {
                const ssize_t w = ::send(fd, frame.constData() + sent,
                                         static_cast<size_t>(frame.size() - sent), MSG_NOSIGNAL);
                if (w <= 0) { if (errno == EINTR) continue; sent = -1; break; }
                sent += w;
            }
            if (sent < 0) { LOG_W(QStringLiteral("send 失败 fd=%1").arg(fd)); goto done; }
        }
        if (parser.overflow()) {     // 长度头越界 → 协议不一致，断开
            LOG_E(QStringLiteral("报文长度越界，断开 fd=%1").arg(fd));
            break;
        }
    }
done:
    removeConn(fd);
    ::close(fd);
}

void TcpServer::stop()
{
    if (!m_running.exchange(false)) return;
    if (m_listenFd >= 0) { ::shutdown(m_listenFd, SHUT_RDWR); ::close(m_listenFd); m_listenFd = -1; }

    // 关键：半关闭所有在用连接，让卡在 recv 上的工作线程立刻返回 0。
    // 只 shutdown 不 close —— fd 由各自的工作线程关，避免 fd 复用错杀。
    pthread_mutex_lock(&m_connMtx);
    for (int fd : m_conns) ::shutdown(fd, SHUT_RDWR);
    const int n = static_cast<int>(m_conns.size());
    pthread_mutex_unlock(&m_connMtx);
    if (n > 0) LOG_I(QStringLiteral("正在断开 %1 条在用连接…").arg(n));

    m_pool.stop();                   // 此时工作线程都能退出来，join 不会卡住

    if (m_wakeFd[0] >= 0) { ::close(m_wakeFd[0]); m_wakeFd[0] = -1; }
    if (m_wakeFd[1] >= 0) { ::close(m_wakeFd[1]); m_wakeFd[1] = -1; }
    LOG_I(QStringLiteral("服务端已停止"));
}

} // namespace ecp
