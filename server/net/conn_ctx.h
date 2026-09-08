#pragma once
// -----------------------------------------------------------------------------
//  server/net/conn_ctx.h  —  单条 TCP 连接的运行时状态
//  归属 L1。
//
//  线程归属：
//   - m_fd：只由 epoll IO 线程读写（recv/send/close），业务线程绝不碰 fd。
//   - m_sendQueue + m_sendMtx：业务线程入队、IO 线程出队，跨线程共享。
//   - parser / sending / sentOff / pileCode：仅 IO 线程访问，不加锁。
//   - m_closed：IO 线程写，任意线程读（原子）。
//
//  使用说明：
//   - 业务线程只调 enqueueSend() 投递待发送帧。
//   - 禁止业务层直接读写 fd、直接调用 recv/send/shutdown。
//
//  依赖：无其他 net模块强依赖；生命周期由std::shared_ptr管理
//  线程：enqueueSend()多线程安全；IO 字段仅允许 epoll‑IO 线程访问
// -----------------------------------------------------------------------------
#include <atomic>
#include <deque>
#include <mutex>

#include <QByteArray>
#include <QString>

#include "frame.h"

namespace ecp {

class ConnectionCtx
{
public:
    explicit ConnectionCtx(int fd) : m_fd(fd) {}

    int  fd() const       { return m_fd; }
    bool isClosed() const { return m_closed.load(std::memory_order_acquire); }

    // 业务线程调用：把 encodeFrame() 后的完整帧压入发送队列。
    // 连接已关闭时直接丢弃并返回 false；队列由空变非空时唤醒 IO 线程重新 arm EPOLLOUT。
    bool enqueueSend(const QByteArray &frame);

    // IO 线程调用：取队头一帧，成功 true，空 false。
    bool popSend(QByteArray &out);
    bool sendQueueEmpty() const;

    // IO 线程调用：标记关闭（幂等）。fd 的 close 由 IO 线程执行。
    void markClosed() { m_closed.store(true, std::memory_order_release); }

    // —— 以下成员仅 IO 线程访问 ——
    FrameParser parser;         // 粘包/半包解析
    QByteArray  sending;        // 当前正在发送的帧（完整帧）
    qint64      sentOff = 0;    // 当前帧已发送偏移（处理半包写）
    QString     pileCode;       // 9001 注册后填入；非设备连接为空

    // 用户鉴权后由 dispatcher 写入（写一次），closeConnection 读取注销；0 = 未登录
    int         userId = 0;

private:
    int                    m_fd;
    std::atomic<bool>      m_closed{false};
    mutable std::mutex     m_sendMtx;
    std::deque<QByteArray> m_sendQueue;
};

} // namespace ecp
