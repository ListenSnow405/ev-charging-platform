// -----------------------------------------------------------------------------
//  server/main.cpp  —  业务服务端入口
//
//  [说明书] 1.6 Socket 通信 + 多线程(pthread) 主框架
//  [说明书] 1.6 QSQLite 数据存储
//
//  运行：./ecp-server [配置文件路径]
//        默认读 config/app.ini，读不到则用内置默认值。
// -----------------------------------------------------------------------------
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <csignal>

#include "logger.h"
#include "protocol.h"
#include "net/tcp_server.h"
#include "net/dispatcher.h"
#include "dao/db.h"
#include "app_path.h"

using namespace ecp;

static TcpServer *g_server = nullptr;

static void onSignal(int sig)
{
    LOG_I(QStringLiteral("收到信号 %1，正在停止服务端…").arg(sig));
    if (g_server) g_server->stop();
}

// -----------------------------------------------------------------------------
//  业务 handler 注册。
//  TODO(L2)：在 server/biz/ 下实现各服务后，在此逐个注册。
//            清单见 server/biz/README.md 与 docs/protocol.md 第 4 节。
// -----------------------------------------------------------------------------
namespace ecp { void registerUserService(); void registerAdminService(); void registerWalletService(); }

static void registerAllServices()
{
    registerUserService();      // 1001 / 1002   [说明书] 1.4 手机号免密登录
    registerAdminService();     // 2001          [说明书] 1.4 管理员登录
    registerWalletService();    // 1005 / 1006   [说明书] 1.4 钱包充值

    // 骨架自带的连通性探针：客户端可用它确认链路打通（不在协议表内，仅供联调）
    Dispatcher::instance().registerHandler(0, [](const Request &, QJsonObject &out) -> int {
        out["pong"]    = true;
        out["service"] = QStringLiteral("ecp-server");
        return ERR_OK;
    }, /*needAuth=*/false);

    LOG_W(QStringLiteral("其余业务 handler 尚未注册 —— 未实现的命令字返回 ERR_CMD_UNKNOWN(1005)"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ecp-server"));

    // 写已关闭的连接会收到 SIGPIPE，默认行为是终止进程 —— 必须忽略
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT,  onSignal);
    ::signal(SIGTERM, onSignal);

    // ---- 读配置 ----
    const QString cfgPath = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                       : resPath(QStringLiteral("config/app.ini"));
    quint16 port     = 9527;   // t_sys_config.server_port
    int     poolSize = 8;      // t_sys_config.thread_pool_size
    QString dbFile   = QStringLiteral("charging.db");

    if (QFileInfo::exists(cfgPath)) {
        QSettings cfg(cfgPath, QSettings::IniFormat);
        port     = static_cast<quint16>(cfg.value(QStringLiteral("server/port"), port).toUInt());
        poolSize = cfg.value(QStringLiteral("server/pool_size"), poolSize).toInt();
        dbFile   = cfg.value(QStringLiteral("server/db"), dbFile).toString();
        dbFile   = resPath(dbFile);
        LOG_I(QStringLiteral("已加载配置 %1").arg(cfgPath));
    } else {
        LOG_W(QStringLiteral("未找到配置 %1，使用默认值（端口 %2）。"
                             "可执行 cp config/app.ini.example config/app.ini").arg(cfgPath).arg(port));
    }

    // ---- 数据库 ----
    dbFile = resPath(dbFile);
    setDbPath(dbFile);
    if (!QFileInfo::exists(dbFile)) {
        LOG_W(QStringLiteral("数据库文件 %1 不存在。请先执行："
                             "sqlite3 %1 < docs/db-schema.sql").arg(dbFile));
    } else {
        QSqlDatabase db = threadDb();      // 主线程试连一次，尽早暴露问题
        if (!db.isOpen()) {
            LOG_E(QStringLiteral("数据库无法打开，服务端退出"));
            return 1;
        }
        LOG_I(QStringLiteral("数据库就绪: %1").arg(dbFile));
    }

    registerAllServices();

    // ---- 启动 ----
    TcpServer server;
    g_server = &server;
    if (!server.listenOn(port, poolSize)) {
        LOG_E(QStringLiteral("服务端启动失败"));
        return 1;
    }
    server.run();          // 阻塞至 stop()
    return 0;
}
