// -----------------------------------------------------------------------------
//  server/net/device_service.cpp  —  设备侧命令字（9001–9004）处理
//  归属 L1。9001/9002/9004 是设备→服务端请求走 registerHandler；
//  9003 是服务端→设备推送，不走 handler，由 2112 调 pushToDevice() 触发。
// -----------------------------------------------------------------------------
#include <QJsonObject>
#include <QList>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "protocol.h"
#include "logger.h"
#include "time_util.h"
#include "net/dispatcher.h"
#include "net/device_registry.h"
#include "dao/db.h"

namespace ecp {

// 按 pileCode 查 pile_id。成功填 out 返回 ERR_OK；查不到 ERR_PILE_NOT_FOUND；DB 错 ERR_INTERNAL。
static int findPileId(const QString &pileCode, qint64 &out)
{
    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT pile_id FROM t_pile WHERE pile_code = ?"));
    q.addBindValue(pileCode);
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询电桩失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!q.next()) return ERR_PILE_NOT_FOUND;
    out = q.value(0).toLongLong();
    return ERR_OK;
}

// 写一条 t_pile_log（设备侧事件，operator 恒为 system）
static void logPileEvent(qint64 pileId, int event, const QString &detail)
{
    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO t_pile_log(pile_id, event, new_status, operator, detail, create_time)"
        " VALUES(?, ?, NULL, 'system', ?, ?)"));
    q.addBindValue(pileId);
    q.addBindValue(event);
    q.addBindValue(detail);
    q.addBindValue(nowStr());
    q.exec();
}

// 9001 注册上线：登记 桩号→连接 映射，置 online=1，写上线日志
static int handleDevRegister(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);
    const QString pileCode = req.data.value("pileCode").toString().trimmed();
    if (pileCode.isEmpty()) return ERR_PARAM;

    qint64 pileId = 0;
    const int r = findPileId(pileCode, pileId);
    if (r != ERR_OK) return r;

    // 关键：把「本条连接」登记进设备注册表，之后 2112 才能按桩号推 9003
    DeviceRegistry::instance().registerDevice(pileCode, req.conn);

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE t_pile SET online = 1, last_heartbeat = ? WHERE pile_id = ?"));
    q.addBindValue(nowStr());
    q.addBindValue(pileId);
    if (!q.exec()) return ERR_INTERNAL;

    logPileEvent(pileId, 0 /*上线*/, QStringLiteral("电桩上线"));
    LOG_I(QStringLiteral("电桩上线: %1").arg(pileCode));
    return ERR_OK;
}

// 9002 状态上报：更新状态，写状态变更/故障日志
static int handleDevReport(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);
    const QString pileCode = req.data.value("pileCode").toString().trimmed();
    if (pileCode.isEmpty()) return ERR_PARAM;

    qint64 pileId = 0;
    const int r = findPileId(pileCode, pileId);
    if (r != ERR_OK) return r;

    const int status = req.data.value("status").toInt(-1);
    if (status != PILE_IN_USE && status != PILE_IDLE && status != PILE_FAULT)
        return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE t_pile SET status = ? WHERE pile_id = ?"));
    q.addBindValue(status);
    q.addBindValue(pileId);
    if (!q.exec()) return ERR_INTERNAL;

    // 故障单独记 event=4，其余记 event=2
    logPileEvent(pileId, status == PILE_FAULT ? 4 : 2,
                 status == PILE_FAULT ? QStringLiteral("故障上报") : QStringLiteral("状态上报"));

    // 更新内存中的最近上报（供 1204 结算取整数 kwhX100）
    DeviceReport report;
    report.status  = status;
    report.kwhX100 = static_cast<qint64>(req.data.value("kwh").toDouble() * 100.0 + 0.5);
    report.power   = req.data.value("power").toDouble();
    DeviceRegistry::instance().updateReport(pileCode, report);
    return ERR_OK;
}

// 9004 心跳：刷 last_heartbeat，顺带确保 online=1；不写日志（避免刷屏）
static int handleDevHeartbeat(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);
    const QString pileCode = req.data.value("pileCode").toString().trimmed();
    if (pileCode.isEmpty()) return ERR_PARAM;

    qint64 pileId = 0;
    const int r = findPileId(pileCode, pileId);
    if (r != ERR_OK) return r;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE t_pile SET online = 1, last_heartbeat = ? WHERE pile_id = ?"));
    q.addBindValue(nowStr());
    q.addBindValue(pileId);
    if (!q.exec()) return ERR_INTERNAL;
    return ERR_OK;
}

// 定时离线判定：超过阈值（15s）没心跳的电桩置离线，写 event=1。
// 由 tcp_server.cpp 的 onSweep() 每 60s 调一次。阈值写死 15s（模拟器 5s 一跳 ×3）。
void sweepOfflineDevices()
{
    const QString cutoff = toStr(QDateTime::currentDateTime().addSecs(-15));
    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return;

    QSqlQuery sel(db);
    sel.prepare(QStringLiteral(
        "SELECT pile_id FROM t_pile WHERE online = 1 AND last_heartbeat < ?"));
    sel.addBindValue(cutoff);
    if (!sel.exec()) return;

    QList<qint64> ids;
    while (sel.next()) ids.append(sel.value(0).toLongLong());
    if (ids.isEmpty()) return;

    for (qint64 id : ids) {
        QSqlQuery upd(db);
        upd.prepare(QStringLiteral("UPDATE t_pile SET online = 0 WHERE pile_id = ?"));
        upd.addBindValue(id);
        upd.exec();
        logPileEvent(id, 1 /*离线*/, QStringLiteral("心跳超时"));
    }
    LOG_I(QStringLiteral("设备离线判定：%1 台电桩置离线").arg(ids.size()));
}

void registerDeviceService()
{
    Dispatcher::instance().registerHandler(CMD_DEV_REGISTER,  handleDevRegister,  /*needAuth=*/false);
    Dispatcher::instance().registerHandler(CMD_DEV_REPORT,    handleDevReport,    /*needAuth=*/false);
    Dispatcher::instance().registerHandler(CMD_DEV_HEARTBEAT, handleDevHeartbeat, /*needAuth=*/false);
    LOG_I(QStringLiteral("设备服务已注册: 9001 上线 / 9002 上报 / 9004 心跳；9003 由 2112 推送触发"));
}

} // namespace ecp
