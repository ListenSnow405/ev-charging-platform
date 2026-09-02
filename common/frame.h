#pragma once
// =============================================================================
//  common/frame.h  —  4 字节长度头 + JSON 体 的帧编解码
//
//  冻结契约 · 属主 L1，其他人只读。变更走 CLAUDE.md 第 3 节流程。
//  [本组自定] 帧格式，对应 docs/protocol.md 第 2 节
//
//  ⚠ CLAUDE.md 硬性规则第 1 条：
//    TCP 是字节流，没有消息边界。禁止假设一次 read() / readyRead() 就是一个完整包。
//    服务端与两个 Qt 客户端 **一律使用本文件**，不要各自手写解析。
//
//  用法（服务端 raw fd 与 Qt 客户端 QTcpSocket 通用）：
//      FrameParser parser;
//      parser.append(刚读到的字节);            // 有多少喂多少，可以是半包也可以是多包
//      QByteArray payload;
//      while (parser.next(payload)) { 处理一条完整 JSON 报文 }
// =============================================================================

#include <QByteArray>
#include <QtEndian>

namespace ecp {

// 单帧 payload 上限 1 MB，超出视为异常连接（docs/protocol.md 第 2 节）
static const int   FRAME_HEADER_LEN = 4;
static const qint64 FRAME_MAX_PAYLOAD = 1024 * 1024;

// ---- 编码：JSON 字节 → 完整帧 -------------------------------------------------
inline QByteArray encodeFrame(const QByteArray &payload)
{
    QByteArray out;
    out.resize(FRAME_HEADER_LEN);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()),
                          reinterpret_cast<uchar *>(out.data()));
    out.append(payload);
    return out;
}

// ---- 解码：增量喂字节，逐条取出完整帧 -------------------------------------------
class FrameParser
{
public:
    // 把新读到的字节追加进缓冲区。可以是半个包、一个包、或好几个包。
    void append(const QByteArray &chunk) { m_buf.append(chunk); }

    // 取出下一条完整报文。返回 false 表示缓冲区里还不够一整帧，继续收。
    bool next(QByteArray &payload)
    {
        if (m_overflow) return false;                       // 已判定异常，不再解析
        if (m_buf.size() < FRAME_HEADER_LEN) return false;  // 连长度头都没收全

        const quint32 len = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar *>(m_buf.constData()));

        if (len > FRAME_MAX_PAYLOAD) {   // 长度异常：多半是对端协议不一致，交由上层断开
            m_overflow = true;
            return false;
        }
        if (m_buf.size() < FRAME_HEADER_LEN + static_cast<int>(len))
            return false;                                   // 半包，等下一次 append

        payload = m_buf.mid(FRAME_HEADER_LEN, static_cast<int>(len));
        m_buf.remove(0, FRAME_HEADER_LEN + static_cast<int>(len));  // 粘包：剩下的留着
        return true;
    }

    // 长度头越界 —— 上层应记日志并关闭连接，返回 ERR_FRAME
    bool overflow() const { return m_overflow; }

    void reset() { m_buf.clear(); m_overflow = false; }

private:
    QByteArray m_buf;
    bool       m_overflow = false;
};

} // namespace ecp
