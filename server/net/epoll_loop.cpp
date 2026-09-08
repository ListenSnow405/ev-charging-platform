#include "epoll_loop.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "device_registry.h"
#include "dispatcher.h"
#include "frame.h"
#include "io_wake.h"
#include "logger.h"
#include "thread_pool.h"

namespace ecp {

EpollLoop::EpollLoop(ThreadPool &pool) : m_pool(pool) {}
EpollLoop::~EpollLoop() { stop(); }

void *EpollLoop::entry(void *arg)
{
    static_cast<EpollLoop *>(arg)->run();
    return nullptr;
}

bool EpollLoop::start()
{
    if (!initIoWakeup()) { LOG_E(QStringLiteral("创建 eventfd 失败")); return false; }
    m_wakeFd = ioWakeupFd();

    m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        LOG_E(QStringLiteral("epoll_create1 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = m_wakeFd;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeFd, &ev) != 0) {
        LOG_E(QStringLiteral("注册 eventfd 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    m_running = true;
    if (pthread_create(&m_thread, nullptr, &EpollLoop::entry, this) != 0) {
        m_running = false;
        LOG_E(QStringLiteral("IO 线程创建失败"));
        return false;
    }
    LOG_I(QStringLiteral("epoll IO 线程已启动"));
    return true;
}

void EpollLoop::addConnection(std::shared_ptr<ConnectionCtx> ctx)
{
    const int fd = ctx->fd();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_conns.insert(fd, ctx);
    }
    epoll_event ev{};
    ev.events  = EPOLLIN;   // 只监听读；写按需 arm
    ev.data.fd = fd;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        LOG_W(QStringLiteral("epoll 加入连接失败 fd=%1").arg(fd));
        { std::lock_guard<std::mutex> lk(m_mtx); m_conns.remove(fd); }
        ::close(fd);
    }
}

void EpollLoop::run()
{
    const int maxEvents = 64;
    epoll_event events[maxEvents];
    while (m_running) {
        const int n = ::epoll_wait(m_epollFd, events, maxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_E(QStringLiteral("epoll_wait 失败: %1").arg(QString::fromLocal8Bit(strerror(errno))));
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == m_wakeFd) { drainWakeup(); continue; }
            handleEvent(events[i].data.fd, events[i].events);
        }
    }

    // 退出清理：关闭全部连接
    std::vector<std::shared_ptr<ConnectionCtx>> conns;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        conns.reserve(static_cast<size_t>(m_conns.size()));
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it) conns.push_back(it.value());
        m_conns.clear();
    }
    for (auto &c : conns) closeConnection(c);
}

void EpollLoop::handleEvent(int fd, uint32_t events)
{
    std::shared_ptr<ConnectionCtx> ctx;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.constFind(fd);
        if (it == m_conns.constEnd()) return;   // 已关闭
        ctx = it.value();
    }

    if (events & (EPOLLERR | EPOLLHUP)) { closeConnection(ctx); return; }
    if (events & EPOLLIN)  handleReadable(ctx);
    if (ctx->isClosed()) return;
    if (events & EPOLLOUT) handleWritable(ctx);
}

void EpollLoop::handleReadable(const std::shared_ptr<ConnectionCtx> &ctx)
{
    const int fd = ctx->fd();
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ctx->parser.append(QByteArray(buf, static_cast<int>(n)));
            QByteArray payload;
            while (ctx->parser.next(payload)) {
                // 完整报文 → 投递业务任务（不在 IO 线程执行）。capture shared_ptr 保证 ctx 存活。
                m_pool.post([ctx, payload] {
                    const QByteArray resp = Dispatcher::instance().handle(payload, ctx);
                    ctx->enqueueSend(encodeFrame(resp));
                });
            }
            if (ctx->parser.overflow()) { closeConnection(ctx); return; }   // 长度越界
            continue;
        }
        if (n == 0) { closeConnection(ctx); return; }                       // 对端关闭
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;      // 读尽
        closeConnection(ctx); return;                                        // 其它错误
    }
}

void EpollLoop::handleWritable(const std::shared_ptr<ConnectionCtx> &ctx)
{
    const int fd = ctx->fd();
    for (;;) {
        // 无当前帧则从队列取新帧
        if (ctx->sentOff >= ctx->sending.size()) {
            QByteArray frame;
            if (!ctx->popSend(frame)) break;   // 队列空，发完
            ctx->sending = std::move(frame);
            ctx->sentOff = 0;
        }
        const char   *p      = ctx->sending.constData() + ctx->sentOff;
        const qint64  remain = ctx->sending.size() - ctx->sentOff;
        const ssize_t n      = ::send(fd, p, static_cast<size_t>(remain), MSG_NOSIGNAL);
        if (n > 0) { ctx->sentOff += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   // 内核缓冲满，等下次 EPOLLOUT
        closeConnection(ctx); return;
    }
    // 队列空且当前帧发完 → 关 EPOLLOUT，避免空转
    if (ctx->sendQueueEmpty() && ctx->sentOff >= ctx->sending.size())
        modEvent(fd, EPOLLIN);
}

void EpollLoop::closeConnection(const std::shared_ptr<ConnectionCtx> &ctx)
{
    if (ctx->isClosed()) return;
    ctx->markClosed();
    const int fd = ctx->fd();
    ::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_conns.remove(fd);
    }
    if (!ctx->pileCode.isEmpty())
        DeviceRegistry::instance().unregisterDevice(ctx->pileCode, ctx.get());
    LOG_I(QStringLiteral("连接关闭 fd=%1").arg(fd));
}

void EpollLoop::modEvent(int fd, uint32_t events)
{
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    ::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev);   // fd 可能已关闭，忽略失败
}

void EpollLoop::drainWakeup()
{
    std::uint64_t x;
    while (::read(m_wakeFd, &x, sizeof(x)) > 0) {}    // 读空 eventfd

    // 唤醒来源：有连接队列空→非空，需重新 arm EPOLLOUT
    std::vector<int> ready;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it)
            if (!it.value()->sendQueueEmpty()) ready.push_back(it.key());
    }
    for (int fd : ready) modEvent(fd, EPOLLIN | EPOLLOUT);
}

void EpollLoop::stop()
{
    if (!m_running.exchange(false)) return;
    wakeIoLoop();   // 唤醒 IO 线程退出
    pthread_join(m_thread, nullptr);
    if (m_epollFd >= 0) { ::close(m_epollFd); m_epollFd = -1; }
    LOG_I(QStringLiteral("epoll IO 线程已停止"));
}

} // namespace ecp
