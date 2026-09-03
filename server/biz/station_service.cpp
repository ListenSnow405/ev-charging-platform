// -----------------------------------------------------------------------------
//  server/biz/station_service.cpp  —  电站服务　归属 L2
//
//  [说明书] 1.4 用户端附近充电站、管理端电站列表与新增电站。
// -----------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "protocol.h"
#include "logger.h"
#include "time_util.h"
#include "net/dispatcher.h"
#include "dao/db.h"

namespace ecp {

static bool positiveInteger(const QJsonValue &value, qint64 &out)
{
    if (!value.isDouble()) return false;
    out = value.toInteger(-1);
    return out > 0;
}

static bool coordinate(const QJsonValue &value, double minimum, double maximum,
                       double &out)
{
    if (!value.isDouble()) return false;
    out = value.toDouble();
    return std::isfinite(out) && out >= minimum && out <= maximum;
}

static void rollback(QSqlDatabase &db)
{
    if (!db.rollback())
        LOG_E(QStringLiteral("电站事务回滚失败: %1").arg(db.lastError().text()));
}

static qint64 distanceMetres(double fromLng, double fromLat, double toLng, double toLat)
{
    constexpr double earthRadiusMetres = 6371000.0;
    constexpr double pi = 3.14159265358979323846;
    const auto radians = [pi](double degrees) { return degrees * pi / 180.0; };

    const double fromLatRad = radians(fromLat);
    const double toLatRad = radians(toLat);
    const double deltaLat = radians(toLat - fromLat);
    const double deltaLng = radians(toLng - fromLng);
    const double sinLat = std::sin(deltaLat / 2.0);
    const double sinLng = std::sin(deltaLng / 2.0);
    const double a = sinLat * sinLat
                     + std::cos(fromLatRad) * std::cos(toLatRad) * sinLng * sinLng;
    const double centralAngle = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return static_cast<qint64>(std::llround(earthRadiusMetres * centralAngle));
}

static int handleNearby(const Request &req, QJsonObject &out)
{
    double lng = 0.0;
    double lat = 0.0;
    if (!coordinate(req.data.value("lng"), -180.0, 180.0, lng)
        || !coordinate(req.data.value("lat"), -90.0, 90.0, lat)) {
        return ERR_PARAM;
    }
    const QJsonValue keywordValue = req.data.value("keyword");
    if (!keywordValue.isString()) return ERR_PARAM;
    const QString pattern = QStringLiteral("%") + keywordValue.toString() + QStringLiteral("%");

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("附近电站获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT s.station_id, s.name, s.address, s.lng, s.lat, s.price,"
        " COUNT(p.pile_id) AS pile_total,"
        " COALESCE(SUM(CASE WHEN p.status = ? AND p.online = 1 THEN 1 ELSE 0 END), 0) AS pile_idle"
        " FROM t_station s LEFT JOIN t_pile p ON p.station_id = s.station_id"
        " WHERE s.status = 0 AND (s.name LIKE ? OR s.address LIKE ?)"
        " GROUP BY s.station_id, s.name, s.address, s.lng, s.lat, s.price"));
    query.addBindValue(PILE_IDLE);
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询附近电站失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    struct NearbyStation { qint64 stationId; qint64 distance; QJsonObject json; };
    std::vector<NearbyStation> stations;
    while (query.next()) {
        const qint64 stationId = query.value("station_id").toLongLong();
        const qint64 distance = distanceMetres(lng, lat, query.value("lng").toDouble(),
                                               query.value("lat").toDouble());
        QJsonObject item;
        item["stationId"] = stationId;
        item["name"] = query.value("name").toString();
        item["address"] = query.value("address").toString();
        item["price"] = query.value("price").toLongLong();
        item["pileTotal"] = query.value("pile_total").toLongLong();
        item["pileIdle"] = query.value("pile_idle").toLongLong();
        item["distance"] = distance;
        stations.push_back({stationId, distance, item});
    }
    std::sort(stations.begin(), stations.end(), [](const NearbyStation &left,
                                                    const NearbyStation &right) {
        if (left.distance != right.distance) return left.distance < right.distance;
        return left.stationId < right.stationId;
    });

    QJsonArray list;
    for (const NearbyStation &station : stations) list.append(station.json);
    out["list"] = list;
    return ERR_OK;
}

static int handleStationList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    if (!positiveInteger(req.data.value("page"), page)
        || !positiveInteger(req.data.value("size"), size)) return ERR_PARAM;
    if (page - 1 > std::numeric_limits<qint64>::max() / size) return ERR_PARAM;
    const qint64 offset = (page - 1) * size;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("电站列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    QSqlQuery count(db);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM t_station"));
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询电站总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取电站总数失败"));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT s.station_id, s.name, s.address, s.lng, s.lat,"
        " COUNT(p.pile_id) AS pile_total,"
        " COALESCE(SUM(CASE WHEN p.online = 1 THEN 1 ELSE 0 END), 0) AS online_count"
        " FROM t_station s LEFT JOIN t_pile p ON p.station_id = s.station_id"
        " GROUP BY s.station_id, s.name, s.address, s.lng, s.lat"
        " ORDER BY s.station_id ASC LIMIT ? OFFSET ?"));
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询电站分页列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        const qint64 pileTotal = query.value("pile_total").toLongLong();
        const qint64 onlineCount = query.value("online_count").toLongLong();
        QJsonObject item;
        item["stationId"] = query.value("station_id").toLongLong();
        item["name"] = query.value("name").toString();
        item["address"] = query.value("address").toString();
        item["lng"] = query.value("lng").toDouble();
        item["lat"] = query.value("lat").toDouble();
        item["pileTotal"] = pileTotal;
        item["onlineRate"] = pileTotal == 0 ? 0.0
                                             : static_cast<double>(onlineCount) * 100.0 / pileTotal;
        list.append(item);
    }
    out["total"] = count.value(0).toLongLong();
    out["list"] = list;
    return ERR_OK;
}

static int handleStationAdd(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QJsonValue nameValue = req.data.value("name");
    const QJsonValue addressValue = req.data.value("address");
    if (!nameValue.isString() || !addressValue.isString()) return ERR_PARAM;
    const QString name = nameValue.toString().trimmed();
    const QString address = addressValue.toString().trimmed();
    if (name.isEmpty() || address.isEmpty()) return ERR_PARAM;

    double lng = 0.0;
    double lat = 0.0;
    qint64 price = 0;
    qint64 pileCount = 0;
    if (!coordinate(req.data.value("lng"), -180.0, 180.0, lng)
        || !coordinate(req.data.value("lat"), -90.0, 90.0, lat)
        || !positiveInteger(req.data.value("price"), price)
        || !positiveInteger(req.data.value("pileCount"), pileCount)
        || pileCount > 99) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("新增电站获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!db.transaction()) {
        LOG_E(QStringLiteral("开启新增电站事务失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    const QString now = nowStr();
    QSqlQuery station(db);
    station.prepare(QStringLiteral(
        "INSERT INTO t_station(name, address, lng, lat, price, status, create_time)"
        " VALUES(?, ?, ?, ?, ?, 0, ?)"));
    station.addBindValue(name);
    station.addBindValue(address);
    station.addBindValue(lng);
    station.addBindValue(lat);
    station.addBindValue(price);
    station.addBindValue(now);
    if (!station.exec()) {
        LOG_E(QStringLiteral("新增电站失败: %1").arg(station.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    const qint64 stationId = station.lastInsertId().toLongLong();
    if (stationId <= 0) {
        LOG_E(QStringLiteral("新增电站未取得有效 stationId: %1").arg(stationId));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (stationId > 999) {
        LOG_W(QStringLiteral("新增电站编号超出三位格式范围: stationId=%1").arg(stationId));
        rollback(db);
        return ERR_PARAM;
    }

    const qint64 fastCount = (pileCount + 1) / 2;
    QSqlQuery pile(db);
    pile.prepare(QStringLiteral(
        "INSERT INTO t_pile(pile_code, station_id, type, power, status, charge_count,"
        " charge_duration, online, create_time) VALUES(?, ?, ?, ?, ?, 0, 0, 0, ?)"));
    for (qint64 sequence = 1; sequence <= pileCount; ++sequence) {
        const int type = sequence <= fastCount ? PILE_FAST : PILE_SLOW;
        const double power = type == PILE_FAST ? 120.0 : 7.0;
        const QString code = QStringLiteral("SZ%1-%2")
                                 .arg(stationId, 3, 10, QLatin1Char('0'))
                                 .arg(sequence, 2, 10, QLatin1Char('0'));
        pile.bindValue(0, code);
        pile.bindValue(1, stationId);
        pile.bindValue(2, type);
        pile.bindValue(3, power);
        pile.bindValue(4, PILE_IDLE);
        pile.bindValue(5, now);
        if (!pile.exec()) {
            LOG_E(QStringLiteral("新增电桩失败: stationId=%1 sequence=%2 error=%3")
                      .arg(stationId).arg(sequence).arg(pile.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
    }

    QSqlQuery oplog(db);
    oplog.prepare(QStringLiteral(
        "INSERT INTO t_admin_oplog(admin_id, action, target, detail, create_time)"
        " VALUES(?, ?, ?, ?, ?)"));
    oplog.addBindValue(req.session.id);
    oplog.addBindValue(QStringLiteral("ADD_STATION"));
    oplog.addBindValue(QString::number(stationId));
    oplog.addBindValue(QStringLiteral("站名=%1，电桩数量=%2").arg(name).arg(pileCount));
    oplog.addBindValue(now);
    if (!oplog.exec()) {
        LOG_E(QStringLiteral("写入新增电站操作日志失败: %1").arg(oplog.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交新增电站事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    out["stationId"] = stationId;
    LOG_I(QStringLiteral("管理员新增电站成功: adminId=%1 stationId=%2 pileCount=%3")
              .arg(req.session.id).arg(stationId).arg(pileCount));
    return ERR_OK;
}

static int handleStationDetail(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;
    qint64 stationId = 0;
    if (!positiveInteger(req.data.value("stationId"), stationId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("电站明细获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    QSqlQuery exists(db);
    exists.prepare(QStringLiteral("SELECT 1 FROM t_station WHERE station_id = ?"));
    exists.addBindValue(stationId);
    if (!exists.exec()) {
        LOG_E(QStringLiteral("确认电站存在失败: %1").arg(exists.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!exists.next()) return ERR_STATION_NOT_FOUND;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT pile_id, pile_code, type, status, power, online, charge_count, charge_duration"
        " FROM t_pile WHERE station_id = ? ORDER BY pile_id ASC"));
    query.addBindValue(stationId);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询站内电桩明细失败: %1").arg(query.lastError().text()));
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
        item["online"] = query.value("online").toInt();
        item["chargeCount"] = query.value("charge_count").toLongLong();
        item["chargeDuration"] = query.value("charge_duration").toLongLong();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

void registerStationService()
{
    Dispatcher::instance().registerHandler(CMD_STATION_NEARBY, handleNearby);
    Dispatcher::instance().registerHandler(CMD_STATION_LIST, handleStationList);
    Dispatcher::instance().registerHandler(CMD_STATION_ADD, handleStationAdd);
    Dispatcher::instance().registerHandler(CMD_STATION_DETAIL, handleStationDetail);
    LOG_I(QStringLiteral("电站服务已注册: 1101 附近电站 / 2101 电站列表 / "
                         "2102 新增电站 / 2103 站内电桩明细"));
}

} // namespace ecp
