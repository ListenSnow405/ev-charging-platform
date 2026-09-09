// -----------------------------------------------------------------------------
//  server/biz/pile_service.cpp  —  电桩服务　归属 L2
//
//  [说明书] 1.4 用户端查看站内电桩、管理端分页查询电桩。
//  2112 远程重启通过 L1 的设备连接映射与指令发送接口下发 9003。
// -----------------------------------------------------------------------------
#include <cmath>
#include <limits>

#include <QJsonArray>
#include <QJsonObject>
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

static bool positiveInteger(const QJsonValue &value, qint64 &out)
{
    if (!value.isDouble()) return false;
    out = value.toInteger(-1);
    return out > 0;
}

static bool strictPositiveInteger(const QJsonValue &value, qint64 &out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0
        || std::floor(number) != number
        || number >= static_cast<double>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    out = static_cast<qint64>(number);
    return out > 0;
}

static void rollback(QSqlDatabase &db)
{
    if (!db.rollback())
        LOG_E(QStringLiteral("远程重启事务回滚失败: %1").arg(db.lastError().text()));
}

static bool optionalStationId(const QJsonObject &data, qint64 &out)
{
    const QJsonValue value = data.value("stationId");
    if (value.isUndefined()) {
        out = 0;
        return true;
    }
    if (!value.isDouble()) return false;
    out = value.toInteger(-1);
    return out >= 0;
}

static bool optionalPileStatus(const QJsonObject &data, qint64 &out)
{
    const QJsonValue value = data.value("status");
    if (value.isUndefined()) {
        out = -1;
        return true;
    }
    if (!value.isDouble()) return false;
    out = value.toInteger(-2);
    return out == -1 || out == PILE_IN_USE || out == PILE_IDLE || out == PILE_FAULT;
}

static int ensureStationExists(QSqlDatabase &db, qint64 stationId)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT 1 FROM t_station WHERE station_id = ?"));
    query.addBindValue(stationId);
    if (!query.exec()) {
        LOG_E(QStringLiteral("确认电站存在失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }
    return query.next() ? ERR_OK : ERR_STATION_NOT_FOUND;
}

static int handleStationPiles(const Request &req, QJsonObject &out)
{
    qint64 stationId = 0;
    if (!positiveInteger(req.data.value("stationId"), stationId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询站内电桩获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    const int stationResult = ensureStationExists(db, stationId);
    if (stationResult != ERR_OK) return stationResult;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT pile_id, pile_code, type, status, power"
        " FROM t_pile WHERE station_id = ? ORDER BY pile_id ASC"));
    query.addBindValue(stationId);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询用户端站内电桩失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["pileId"] = query.value("pile_id").toLongLong();
        item["code"] = query.value("pile_code").toString();
        item["type"] = query.value("type").toInt();
        item["status"] = query.value("status").toInt();
        item["power"] = query.value("power").toDouble();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

static int handlePileList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    qint64 stationId = 0;
    qint64 status = -1;
    if (!positiveInteger(req.data.value("page"), page)
        || !positiveInteger(req.data.value("size"), size)
        || !optionalStationId(req.data, stationId)
        || !optionalPileStatus(req.data, status)) {
        return ERR_PARAM;
    }
    if (page - 1 > std::numeric_limits<qint64>::max() / size) return ERR_PARAM;
    const qint64 offset = (page - 1) * size;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("管理端电桩列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    if (stationId != 0) {
        const int stationResult = ensureStationExists(db, stationId);
        if (stationResult != ERR_OK) return stationResult;
    }

    QSqlQuery count(db);
    count.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM t_pile p"
        " WHERE (? = 0 OR p.station_id = ?) AND (? = -1 OR p.status = ?)"));
    count.addBindValue(stationId);
    count.addBindValue(stationId);
    count.addBindValue(status);
    count.addBindValue(status);
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询电桩总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取电桩总数失败"));
        return ERR_INTERNAL;
    }
    const qint64 total = count.value(0).toLongLong();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT p.pile_id, p.pile_code, s.name AS station_name, p.type, p.power,"
        " p.status, p.charge_count, p.charge_duration"
        " FROM t_pile p JOIN t_station s ON s.station_id = p.station_id"
        " WHERE (? = 0 OR p.station_id = ?) AND (? = -1 OR p.status = ?)"
        " ORDER BY p.pile_id ASC LIMIT ? OFFSET ?"));
    query.addBindValue(stationId);
    query.addBindValue(stationId);
    query.addBindValue(status);
    query.addBindValue(status);
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询管理端电桩列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["pileId"] = query.value("pile_id").toLongLong();
        item["code"] = query.value("pile_code").toString();
        item["stationName"] = query.value("station_name").toString();
        item["type"] = query.value("type").toInt();
        item["power"] = query.value("power").toDouble();
        item["status"] = query.value("status").toInt();
        item["chargeCount"] = query.value("charge_count").toLongLong();
        item["chargeDuration"] = query.value("charge_duration").toLongLong();
        list.append(item);
    }

    out["total"] = total;
    out["list"] = list;
    return ERR_OK;
}

static int handlePileReboot(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 pileId = 0;
    if (!strictPositiveInteger(req.data.value("pileId"), pileId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("远程重启获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启远程重启事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery pile(db);
    pile.prepare(QStringLiteral(
        "SELECT pile_id, pile_code, status FROM t_pile WHERE pile_id = ?"));
    pile.addBindValue(pileId);
    if (!pile.exec()) {
        LOG_E(QStringLiteral("查询远程重启电桩失败: %1").arg(pile.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!pile.next()) {
        rollback(db);
        return ERR_PILE_NOT_FOUND;
    }
    const QString pileCode = pile.value("pile_code").toString();
    const int status = pile.value("status").toInt();
    Q_UNUSED(status);

    if (!DeviceRegistry::instance().isOnline(pileCode)) {
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    QSqlQuery admin(db);
    admin.prepare(QStringLiteral("SELECT account FROM t_admin WHERE admin_id = ?"));
    admin.addBindValue(req.session.id);
    if (!admin.exec()) {
        LOG_E(QStringLiteral("查询远程重启管理员失败: %1").arg(admin.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!admin.next()) {
        LOG_E(QStringLiteral("远程重启会话管理员不存在: %1").arg(req.session.id));
        rollback(db);
        return ERR_INTERNAL;
    }
    const QString operatorAccount = admin.value("account").toString();
    const QString now = nowStr();
    const QString detail = QStringLiteral("远程重启电桩 %1").arg(pileCode);

    QSqlQuery pileLog(db);
    pileLog.prepare(QStringLiteral(
        "INSERT INTO t_pile_log(pile_id, event, old_status, new_status, operator, detail, create_time)"
        " VALUES(?, ?, NULL, NULL, ?, ?, ?)"));
    pileLog.addBindValue(pileId);
    pileLog.addBindValue(3);
    pileLog.addBindValue(operatorAccount);
    pileLog.addBindValue(detail);
    pileLog.addBindValue(now);
    if (!pileLog.exec() || pileLog.numRowsAffected() != 1) {
        LOG_E(QStringLiteral("写入远程重启电桩日志失败: %1").arg(pileLog.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    QSqlQuery adminLog(db);
    adminLog.prepare(QStringLiteral(
        "INSERT INTO t_admin_oplog(admin_id, action, target, detail, create_time)"
        " VALUES(?, ?, ?, ?, ?)"));
    adminLog.addBindValue(req.session.id);
    adminLog.addBindValue(QStringLiteral("REBOOT_PILE"));
    adminLog.addBindValue(QString::number(pileId));
    adminLog.addBindValue(detail);
    adminLog.addBindValue(now);
    if (!adminLog.exec() || adminLog.numRowsAffected() != 1) {
        LOG_E(QStringLiteral("写入远程重启管理员日志失败: %1").arg(adminLog.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!pushToDevice(CMD_DEV_REBOOT, pileCode)) {
        LOG_W(QStringLiteral("远程重启下发失败，电桩离线: %1").arg(pileCode));
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交远程重启事务失败，9003可能已下发: %1")
                  .arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    LOG_I(QStringLiteral("管理员远程重启电桩成功: adminId=%1 pileId=%2 pileCode=%3")
              .arg(req.session.id).arg(pileId).arg(pileCode));
    return ERR_OK;
}

void registerPileService()
{
    Dispatcher::instance().registerHandler(CMD_STATION_PILES, handleStationPiles);
    Dispatcher::instance().registerHandler(CMD_PILE_LIST, handlePileList);
    Dispatcher::instance().registerHandler(CMD_PILE_REBOOT, handlePileReboot);
    LOG_I(QStringLiteral("电桩服务已注册: 1102 站内电桩详情 / 2111 管理端电桩列表；"
                         "2112 远程重启"));
}

} // namespace ecp
