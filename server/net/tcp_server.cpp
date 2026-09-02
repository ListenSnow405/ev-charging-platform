#include "tcp_server.h"
#include "dispatcher.h"
#include "frame.h"
#include "logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ecp {

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
    if (!m_pool.start(poolSize)) return false;

    m_running = true;
    LOG_I(QStringLiteral("服务端已监听 0.0.0.0:%1，线程池 %2").arg(port).arg(poolSize));
    return true;
}

void TcpServer::run()
{
    while (m_running) {
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

void TcpServer::serveConnection(int fd)
{
    FrameParser parser;              // ⚠ 每条连接一个解析器，负责处理粘包/半包
    char buf[4096];

    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) { LOG_I(QStringLiteral("对端关闭 fd=%1").arg(fd)); break; }
        if (n < 0) {
            if (errno == EINTR) continue;
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
    ::close(fd);
}

void TcpServer::stop()
{
    if (!m_running.exchange(false)) return;
    if (m_listenFd >= 0) { ::shutdown(m_listenFd, SHUT_RDWR); ::close(m_listenFd); m_listenFd = -1; }
    m_pool.stop();
    LOG_I(QStringLiteral("服务端已停止"));
}

} // namespace ecp
