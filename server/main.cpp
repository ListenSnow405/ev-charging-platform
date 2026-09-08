// -----------------------------------------------------------------------------
//  server/main.cpp  —  业务服务端入口（改造后）
//
//  [说明书] 1.6 Socket 通信 + 多线程(pthread) 主框架
//  [说明书] 1.6 QSQLite 数据存储
//
//  运行：./ecp-server [配置文件路径]
//  主线程跑 Qt 事件循环：QSocketNotifier 接 listen fd、QTimer 轮询退出标志。
// -----------------------------------------------------------------------------
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QVariant>
#include <atomic>
#include <csignal>

#include "logger.h"
#include "error_code_ext.h"
#include "protocol.h"
#include "net/tcp_server.h"
#include "net/dispatcher.h"
#include "dao/db.h"
#include "app_path.h"

using namespace ecp;

// 信号处理函数里只做 async-signal-safe 的 atomic 置位，真正停止回到主线程。
static std::atomic<bool> g_quitRequested{false};

static void onSignal(int) { g_quitRequested.store(true); }

// -----------------------------------------------------------------------------
//  业务 handler 注册。
//  TODO(L2)：在 server/biz/ 下实现各服务后，在此逐个注册。
//            清单见 server/biz/README.md 与 docs/protocol.md 第 4 节。
// -----------------------------------------------------------------------------
namespace ecp { void registerUserService(); void registerAdminService(); void registerWalletService(); void registerUserManagementService(); void registerStationService(); void registerPileService(); void registerReservationService(); void registerOrderService(); void registerStatisticsService(); }

// 扩展模块（加分项）注册入口，各自独立文件，见 docs/expand/00 第 5.2 节
namespace ecp { void registerExt08CarbonService(); }

// -----------------------------------------------------------------------------
//  B4 · 扩展模块功能开关（docs/expand/00 第 4.7 节）　主责 L2，由 08 认领人代实现
//
//  关闭的模块干脆不注册 handler，客户端调用即得 ERR_CMD_UNKNOWN(1005)。
//  这比返回一个新错误码更省事，也天然满足「扩展全部关掉后核心闭环无回归」。
//
//  ⚠ 读不到配置一律按**关闭**处理：宁可不显示功能，也不要显示一个必然报错的入口。
//  ⚠ 必须在数据库就绪之后构造 —— registerAllServices() 在 main() 里正是这个位置。
//    数据库没就绪意味着**所有**扩展被静默关掉，这不是「警告」级别的事，记 LOG_E。
//
//  一次查询读回全部 feat_* 键，而不是每个模块各查一次：十个扩展就是十次往返，
//  日志里还会散落十行，看不出「这次到底开了哪些」。缓存后只读，天然线程安全。
// -----------------------------------------------------------------------------
class FeatureFlags
{
public:
    FeatureFlags()
    {
        QSqlDatabase db = threadDb();
        if (!db.isOpen()) {
            LOG_E(QStringLiteral("功能开关无法读取（数据库未就绪），"
                                 "全部扩展模块按关闭处理: %1").arg(db.lastError().text()));
            return;
        }
        QSqlQuery q(db);
        // 前缀匹配 feat_，各扩展模块的键都落在这个命名空间里（00 第 4.7 节）
        q.prepare(QStringLiteral(
            "SELECT cfg_key, cfg_value FROM t_sys_config WHERE cfg_key LIKE 'feat\\_%' ESCAPE '\\'"));
        if (!q.exec()) {
            LOG_E(QStringLiteral("功能开关查询失败，全部扩展模块按关闭处理: %1")
                      .arg(q.lastError().text()));
            return;
        }
        while (q.next())
            m_flags.insert(q.value(0).toString(), q.value(1).toString().trimmed()
                                                      == QLatin1String("1"));
        m_loaded = true;
    }

    bool enabled(const QString &key) const
    {
        if (!m_loaded) return false;
        if (!m_flags.contains(key)) {
            LOG_W(QStringLiteral("功能开关 %1 不存在于 t_sys_config，按关闭处理。"
                                 "请确认 docs/db-schema-ext-*.sql 已执行").arg(key));
            return false;
        }
        return m_flags.value(key);
    }

    // 一行说清这次开了哪些、关了哪些 —— 排查「功能怎么不见了」从这行开始
    QString summary() const
    {
        if (!m_loaded) return QStringLiteral("扩展模块开关: 读取失败，全部按关闭处理");
        if (m_flags.isEmpty())
            return QStringLiteral("扩展模块开关: t_sys_config 中没有 feat_* 键，全部关闭");
        QStringList on, off;
        for (auto it = m_flags.constBegin(); it != m_flags.constEnd(); ++it)
            (it.value() ? on : off).append(it.key());
        on.sort();
        off.sort();
        return QStringLiteral("扩展模块开关: 开启 %1 个 [%2]　关闭 %3 个 [%4]")
                   .arg(on.size()).arg(on.join(QStringLiteral(", ")))
                   .arg(off.size()).arg(off.join(QStringLiteral(", ")));
    }

private:
    QHash<QString, bool> m_flags;
    bool m_loaded = false;
};

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
    // 新增扩展模块时在这里加一行即可，开关键统一走 feat_<模块号>_<名字>
    const FeatureFlags features;                 // 一次查询读回全部 feat_* 键
    LOG_I(features.summary());
    if (features.enabled(QStringLiteral("feat_08_carbon"))) registerExt08CarbonService();  // 3740–3746
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

    // ---- 扩展错误码文案 ----
    // 让 buildResponse() 的 msg 对 6000 段也返回中文（common/error_code.h 的挂钩）。
    // 必须在起线程池之前注册：它是进程级全局量，此后只读。
    registerExtMsgProvider(&errMsgExt);

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

    // 退出：信号处理器只置 atomic 标志，由 Qt 事件循环里的定时器轮询并优雅停止
    QTimer quitTimer;
    QObject::connect(&quitTimer, &QTimer::timeout, &app, [&] {
        if (g_quitRequested.load()) { server.stop(); app.quit(); }
    });
    quitTimer.start(200);

    return app.exec();
}
