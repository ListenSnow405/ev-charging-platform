#include "conn_ctx.h"
#include "io_wake.h"

namespace ecp {

bool ConnectionCtx::enqueueSend(const QByteArray &frame)
{
    if (frame.isEmpty()) return false;
    std::lock_guard<std::mutex> lk(m_sendMtx);
    if (m_closed.load(std::memory_order_acquire)) return false;
    const bool wasEmpty = m_sendQueue.empty();
    m_sendQueue.push_back(frame);
    if (wasEmpty) wakeIoLoop();   // 队列空→非空，唤醒 IO 线程重新 arm EPOLLOUT
    return true;
}

bool ConnectionCtx::popSend(QByteArray &out)
{
    std::lock_guard<std::mutex> lk(m_sendMtx);
    if (m_sendQueue.empty()) return false;
    out = std::move(m_sendQueue.front());
    m_sendQueue.pop_front();
    return true;
}

bool ConnectionCtx::sendQueueEmpty() const
{
    std::lock_guard<std::mutex> lk(m_sendMtx);
    return m_sendQueue.empty();
}

} // namespace ecp
