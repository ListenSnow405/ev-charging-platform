#pragma once
// -----------------------------------------------------------------------------
//  server/net/dispatcher.h  —  命令字分发
//  归属 L1（框架） / L2（注册各业务 handler）
//
//  docs/protocol.md 第 3 节：请求信封 {cmd, seq, token, data}
//  未注册的命令字统一返回 ERR_CMD_UNKNOWN。
// -----------------------------------------------------------------------------
#include <QHash>
#include <QJsonObject>
#include <functional>
#include "session.h"

namespace ecp {

struct Request {
    int         cmd  = 0;
    int         seq  = 0;
    QString     token;
    QJsonObject data;
    SessionInfo session;      // 已鉴权时有效
    bool        authed = false;
};

// handler 返回错误码，通过 out 填充响应 data
using Handler = std::function<int(const Request &, QJsonObject &out)>;

class Dispatcher
{
public:
    static Dispatcher &instance();

    // needAuth=false 的命令（登录等）不校验 token
    void registerHandler(int cmd, Handler h, bool needAuth = true);

    // 处理一条完整报文，返回待发送的响应 payload
    QByteArray handle(const QByteArray &payload);

private:
    Dispatcher() = default;
    QHash<int, Handler> m_handlers;
    QHash<int, bool>    m_needAuth;
};

} // namespace ecp
