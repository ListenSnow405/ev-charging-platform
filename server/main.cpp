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
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <csignal>
#include <unistd.h>

#include "logger.h"
#include "protocol.h"
#include "net/tcp_server.h"
#include "net/dispatcher.h"
#include "dao/db.h"
#include "app_path.h"

using namespace ecp;

// 信号处理函数里只能碰 volatile sig_atomic_t 和 async-signal-safe 的调用。
// 以前这里直接调 TcpServer::stop()，它要加锁还要 pthread_join，一旦有工作线程
// 卡在 recv 上就永远 join 不回来，主线程被锁死在信号上下文里，进程关不掉。
// 现在只往 self-pipe 写一字节唤醒 accept 循环，真正的停止流程回到主线程做。
static volatile sig_atomic_t g_wakeFd  = -1;
static volatile sig_atomic_t g_lastSig = 0;

static void onSignal(int sig)
{
    g_lastSig = sig;
    if (g_wakeFd >= 0) {
        const char b = 1;
        const ssize_t ignored = ::write(g_wakeFd, &b, 1);   // write 是 async-signal-safe 的
        (void)ignored;
    }
}

// -----------------------------------------------------------------------------
//  业务 handler 注册。
//  TODO(L2)：在 server/biz/ 下实现各服务后，在此逐个注册。
//            清单见 server/biz/README.md 与 docs/protocol.md 第 4 节。
// -----------------------------------------------------------------------------
namespace ecp { void registerUserService(); void registerAdminService(); void registerWalletService(); void registerUserManagementService(); void registerStationService(); void registerPileService(); void registerReservationService(); void registerOrderService(); void registerStatisticsService(); }

// 扩展模块（加分项）注册入口，各自独立文件，见 docs/expand/00 第 5.2 节
namespace ecp { void registerExt08CarbonService(); }

// -----------------------------------------------------------------------------
//  B4 · 扩展模块功能开关（docs/expand/00 第 4.7 节）
//
//  关闭的模块干脆不注册 handler，客户端调用即得 ERR_CMD_UNKNOWN(1005)。
//  这比返回一个新错误码更省事，也天然满足「扩展全部关掉后核心闭环无回归」。
//
//  ⚠ 读不到配置一律按**关闭**处理：宁可不显示功能，也不要显示一个必然报错的入口。
//  ⚠ 必须在数据库就绪之后调用 —— registerAllServices() 在 main() 里正是这个位置。
// -----------------------------------------------------------------------------
static bool featureEnabled(const QString &key)
{
    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_W(QStringLiteral("功能开关 %1 无法读取（数据库未就绪），按关闭处理").arg(key));
        return false;
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT cfg_value FROM t_sys_config WHERE cfg_key = ?"));
    q.addBindValue(key);
    if (!q.exec()) {
        LOG_W(QStringLiteral("功能开关 %1 查询失败，按关闭处理: %2").arg(key, q.lastError().text()));
        return false;
    }
    if (!q.next()) {
        LOG_W(QStringLiteral("功能开关 %1 不存在于 t_sys_config，按关闭处理。"
                             "请确认 docs/db-schema-ext-*.sql 已执行").arg(key));
        return false;
    }
    return q.value(0).toString().trimmed() == QLatin1String("1");
}

static void registerAllServices()
{
    registerUserService();      // 1001 / 1002   [说明书] 1.4 手机号免密登录
    registerAdminService();     // 2001          [说明书] 1.4 管理员登录
    registerWalletService();    // 1005 / 1006   [说明书] 1.4 钱包充值
    registerUserManagementService(); // 2201 / 2202 [说明书] 1.4 用户管理
    registerStationService();   // 1101 / 2101–2103 [说明书] 1.4 电站管理
    registerPileService();      // 1102 / 2111   [说明书] 1.4 电桩查询
    registerReservationService(); // 1202 / 1206 [说明书] 1.4 预约充电
    registerOrderService();     // 1201 / 1207 / 2304 [说明书] 1.4 订单查询
    registerStatisticsService(); // 2301–2303 [说明书] 1.4 营收与电桩状态统计

    // ---- 扩展模块（加分项）· 受 t_sys_config 功能开关控制 ----
    if (featureEnabled(QStringLiteral("feat_08_carbon"))) registerExt08CarbonService();  // 3740–3746
    else LOG_I(QStringLiteral("扩展模块 08 碳减排：feat_08_carbon 未开启，跳过注册"));

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
    if (!server.listenOn(port, poolSize)) {
        LOG_E(QStringLiteral("服务端启动失败"));
        return 1;
    }
    g_wakeFd = server.wakeFd();   // 必须在 listenOn 之后：self-pipe 在那里才建好

    server.run();                 // 阻塞至收到停止信号

    // 回到主线程再做清理：断开在用连接 → 工作线程退出 → join 返回
    if (g_lastSig != 0)
        LOG_I(QStringLiteral("收到信号 %1，正在停止服务端…").arg(static_cast<int>(g_lastSig)));
    g_wakeFd = -1;
    server.stop();
    return 0;
}
