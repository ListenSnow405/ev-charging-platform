// -----------------------------------------------------------------------------
//  server/biz/statistics_service.cpp  —  统计服务　归属 L2
//
//  [说明书] 1.4 管理端今日/本月/总营收、近 7/30 日趋势和电桩状态分布。
// -----------------------------------------------------------------------------
#include <QDate>
#include <QHash>
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

static int handleRevenue(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("营收概览获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    const QDate today = QDate::currentDate();
    const QString todayStart = today.toString(QStringLiteral("yyyy-MM-dd"));
    const QString tomorrowStart = today.addDays(1).toString(QStringLiteral("yyyy-MM-dd"));
    const QDate monthStartDate(today.year(), today.month(), 1);
    const QString monthStart = monthStartDate.toString(QStringLiteral("yyyy-MM-dd"));
    const QString nextMonthStart = monthStartDate.addMonths(1).toString(QStringLiteral("yyyy-MM-dd"));

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT"
        " COALESCE(SUM(CASE WHEN settle_time >= ? AND settle_time < ? THEN amount ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN settle_time >= ? AND settle_time < ? THEN amount ELSE 0 END), 0),"
        " COALESCE(SUM(amount), 0)"
        " FROM t_order WHERE status = ?"));
    query.addBindValue(todayStart);
    query.addBindValue(tomorrowStart);
    query.addBindValue(monthStart);
    query.addBindValue(nextMonthStart);
    query.addBindValue(ORDER_SETTLED);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询营收概览失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        LOG_E(QStringLiteral("读取营收概览失败"));
        return ERR_INTERNAL;
    }

    const qint64 todayAmount = query.value(0).toLongLong();
    const qint64 monthAmount = query.value(1).toLongLong();
    const qint64 totalAmount = query.value(2).toLongLong();
    out["today"] = todayAmount;
    out["month"] = monthAmount;
    out["total"] = totalAmount;
    return ERR_OK;
}

static int handleRevenueTrend(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QJsonValue daysValue = req.data.value("days");
    if (!daysValue.isDouble()) return ERR_PARAM;
    const qint64 days = daysValue.toInteger(-1);
    if (days != 7 && days != 30) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("营收趋势获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    const QDate endDate = QDate::currentDate();
    const QDate startDate = endDate.addDays(1 - days);
    const QString start = startDate.toString(QStringLiteral("yyyy-MM-dd"));
    const QString end = endDate.toString(QStringLiteral("yyyy-MM-dd"));

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT date(settle_time) AS settle_date, COALESCE(SUM(amount), 0) AS amount"
        " FROM t_order"
        " WHERE status = ? AND date(settle_time) >= ? AND date(settle_time) <= ?"
        " GROUP BY date(settle_time) ORDER BY settle_date ASC"));
    query.addBindValue(ORDER_SETTLED);
    query.addBindValue(start);
    query.addBindValue(end);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询营收趋势失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QHash<QString, qint64> amountByDate;
    while (query.next())
        amountByDate.insert(query.value("settle_date").toString(),
                            query.value("amount").toLongLong());

    QJsonArray list;
    for (qint64 i = 0; i < days; ++i) {
        const QString date = startDate.addDays(i).toString(QStringLiteral("yyyy-MM-dd"));
        QJsonObject item;
        item["date"] = date;
        item["amount"] = amountByDate.value(date, 0);
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

static int handlePileStatus(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("电桩状态统计获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT"
        " COALESCE(SUM(CASE WHEN status = ? THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status = ? THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status = ? THEN 1 ELSE 0 END), 0),"
        " COUNT(*) FROM t_pile"));
    query.addBindValue(PILE_IN_USE);
    query.addBindValue(PILE_IDLE);
    query.addBindValue(PILE_FAULT);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询电桩状态统计失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        LOG_E(QStringLiteral("读取电桩状态统计失败"));
        return ERR_INTERNAL;
    }

    const qint64 inUse = query.value(0).toLongLong();
    const qint64 idle = query.value(1).toLongLong();
    const qint64 fault = query.value(2).toLongLong();
    const qint64 total = query.value(3).toLongLong();
    out["inUse"] = inUse;
    out["idle"] = idle;
    out["fault"] = fault;
    out["total"] = total;
    return ERR_OK;
}

static int handleLoadForecast(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QJsonValue stationIdValue = req.data.value("stationId");
    const QJsonValue horizonValue = req.data.value("horizon");
    if (!stationIdValue.isDouble() || !horizonValue.isDouble()) return ERR_PARAM;

    const qint64 stationId = stationIdValue.toInteger(-1);
    const qint64 horizon = horizonValue.toInteger(-1);
    if (stationId < 0 || (horizon != 1 && horizon != 6 && horizon != 24))
        return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("负荷预测获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    if (stationId > 0) {
        QSqlQuery station(db);
        station.prepare(QStringLiteral("SELECT 1 FROM t_station WHERE station_id = ?"));
        station.addBindValue(stationId);
        if (!station.exec()) {
            LOG_E(QStringLiteral("确认负荷预测站点存在失败: %1")
                      .arg(station.lastError().text()));
            return ERR_INTERNAL;
        }
        if (!station.next()) return ERR_STATION_NOT_FOUND;
    }

    QJsonArray list;
    out["list"] = list;

    QSqlQuery latest(db);
    latest.prepare(QStringLiteral(
        "SELECT model_version, create_time FROM t_load_forecast"
        " ORDER BY create_time DESC, forecast_id DESC LIMIT 1"));
    if (!latest.exec()) {
        LOG_E(QStringLiteral("查询最新负荷预测批次失败: %1").arg(latest.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!latest.next()) return ERR_OK;

    const QString modelVersion = latest.value("model_version").toString();
    const QString createTime = latest.value("create_time").toString();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT f.station_id, s.name AS station_name, f.horizon, f.predict_time,"
        " f.load_kw, f.idle_pile, f.is_peak, f.congestion, f.model_version"
        " FROM t_load_forecast f"
        " JOIN t_station s ON s.station_id = f.station_id"
        " WHERE f.model_version = ? AND f.create_time = ? AND f.horizon = ?"
        " AND (? = 0 OR f.station_id = ?)"
        " ORDER BY f.station_id ASC, f.predict_time ASC, f.forecast_id ASC"));
    query.addBindValue(modelVersion);
    query.addBindValue(createTime);
    query.addBindValue(horizon);
    query.addBindValue(stationId);
    query.addBindValue(stationId);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询负荷预测失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    while (query.next()) {
        QJsonObject item;
        item["stationId"] = query.value("station_id").toLongLong();
        item["stationName"] = query.value("station_name").toString();
        item["horizon"] = query.value("horizon").toInt();
        item["predictTime"] = query.value("predict_time").toString();
        item["loadKw"] = query.value("load_kw").toDouble();
        item["idlePile"] = query.value("idle_pile").toLongLong();
        item["isPeak"] = query.value("is_peak").toInt();
        item["congestion"] = query.value("congestion").toDouble();
        item["modelVersion"] = query.value("model_version").toString();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

void registerStatisticsService()
{
    Dispatcher::instance().registerHandler(CMD_STAT_REVENUE, handleRevenue);
    Dispatcher::instance().registerHandler(CMD_STAT_REVENUE_TREND, handleRevenueTrend);
    Dispatcher::instance().registerHandler(CMD_STAT_PILE_STATUS, handlePileStatus);
    Dispatcher::instance().registerHandler(CMD_STAT_LOAD_FORECAST, handleLoadForecast);
    LOG_I(QStringLiteral("统计服务已注册: 2301 营收概览 / 2302 营收趋势 / "
                         "2303 电桩状态分布 / 2305 站点负荷预测"));
}

} // namespace ecp
