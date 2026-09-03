// -----------------------------------------------------------------------------
//  server/biz/pile_service.cpp  —  电桩服务　归属 L2
//
//  [说明书] 1.4 用户端查看站内电桩、管理端分页查询电桩。
//  2112 远程重启依赖 L1 的设备连接映射与指令发送接口，接口就绪前不注册。
// -----------------------------------------------------------------------------
#include <limits>

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "protocol.h"
#include "logger.h"
#include "net/dispatcher.h"
#include "dao/db.h"

namespace ecp {

static bool positiveInteger(const QJsonValue &value, qint64 &out)
{
    if (!value.isDouble()) return false;
    out = value.toInteger(-1);
    return out > 0;
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

void registerPileService()
{
    Dispatcher::instance().registerHandler(CMD_STATION_PILES, handleStationPiles);
    Dispatcher::instance().registerHandler(CMD_PILE_LIST, handlePileList);
    LOG_I(QStringLiteral("电桩服务已注册: 1102 站内电桩详情 / 2111 管理端电桩列表；"
                         "2112 等待 L1 设备指令发送接口"));
}

} // namespace ecp
