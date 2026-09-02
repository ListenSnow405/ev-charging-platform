#include "dispatcher.h"
#include "protocol.h"
#include "logger.h"

namespace ecp {

Dispatcher &Dispatcher::instance()
{
    static Dispatcher d;
    return d;
}

void Dispatcher::registerHandler(int cmd, Handler h, bool needAuth)
{
    m_handlers.insert(cmd, std::move(h));
    m_needAuth.insert(cmd, needAuth);
}

QByteArray Dispatcher::handle(const QByteArray &payload)
{
    QJsonObject env;
    if (!parseEnvelope(payload, env)) {
        LOG_W(QStringLiteral("报文解析失败，长度 %1").arg(payload.size()));
        return buildResponse(0, 0, ERR_FRAME);
    }

    Request req;
    req.cmd   = env.value("cmd").toInt();
    req.seq   = env.value("seq").toInt();
    req.token = env.value("token").toString();
    req.data  = env.value("data").toObject();

    auto it = m_handlers.find(req.cmd);
    if (it == m_handlers.end()) {
        LOG_W(QStringLiteral("未知命令字 %1").arg(req.cmd));
        return buildResponse(req.cmd, req.seq, ERR_CMD_UNKNOWN);
    }

    if (m_needAuth.value(req.cmd, true)) {
        if (req.token.isEmpty())
            return buildResponse(req.cmd, req.seq, ERR_NOT_LOGIN);
        if (!SessionTable::instance().validate(req.token, req.session))
            return buildResponse(req.cmd, req.seq, ERR_TOKEN_INVALID);
        req.authed = true;
    }

    QJsonObject out;
    int code = ERR_INTERNAL;
    try {
        code = (*it)(req, out);
    } catch (...) {
        // [说明书] 2.3 完整的错误处理机制：不允许异常穿透到网络层导致连接断开
        LOG_E(QStringLiteral("handler 抛出异常，cmd=%1").arg(req.cmd));
        code = ERR_INTERNAL;
    }
    return buildResponse(req.cmd, req.seq, code, out);
}

} // namespace ecp
