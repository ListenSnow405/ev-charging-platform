#pragma once
// -----------------------------------------------------------------------------
//  server/net/dispatcher.h  —  命令字分发
//  归属 L1（框架）/ L2（注册）。
//
//  使用说明：
//   - L2 可调用 registerHandler() 注册，鉴权由 dispatcher 自动做，handler 只读 req.session。
//   - 禁止 handler 内部阻塞过长时间；禁止 handler 直接操作 socket fd。
//
//  依赖：frame 帧解析、protocol；业务 biz、device_service 向它注册 handler
//  线程：handle()运行在线程池工作线程
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

    // 已注册的 handler 数量，供启动日志汇总实际注册结果。
    int handlerCount() const { return m_handlers.size(); }

private:
    Dispatcher() = default;
    QHash<int, Handler> m_handlers;
    QHash<int, bool>    m_needAuth;
};

} // namespace ecp
