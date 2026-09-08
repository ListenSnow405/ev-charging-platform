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
#include <memory>
#include "session.h"
#include "conn_ctx.h"

namespace ecp {

struct Request {
    int         cmd  = 0;
    int         seq  = 0;
    QString     token;
    QJsonObject data;
    SessionInfo session;      // 已鉴权时有效
    bool        authed = false;

    // 本条请求所属连接（非拥有）。仅本任务处理期间有效，
    // 供 9001 设备注册等需要回连自身连接的 handler 使用。
    std::shared_ptr<ConnectionCtx> conn;
};

// handler 返回错误码，通过 out 填充响应 data
using Handler = std::function<int(const Request &, QJsonObject &out)>;

class Dispatcher
{
public:
    static Dispatcher &instance();

    // needAuth=false 的命令（登录等）不校验 token
    void registerHandler(int cmd, Handler h, bool needAuth = true);

    // 处理一条完整报文，返回待发送的响应 payload。
    // conn 为请求所属连接，handler 可通过 req.conn 取用（可为空）。
    QByteArray handle(const QByteArray &payload, std::shared_ptr<ConnectionCtx> conn);

private:
    Dispatcher() = default;
    QHash<int, Handler> m_handlers;
    QHash<int, bool>    m_needAuth;
};

} // namespace ecp
