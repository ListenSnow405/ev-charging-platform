// -----------------------------------------------------------------------------
//  server/biz/order_service.cpp  —  订单查询服务　归属 L2
//
//  [说明书] 1.4 用户端订单查询与管理端订单查询。
// -----------------------------------------------------------------------------
#include <limits>

#include <QDateTime>
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

static bool optionalOrderStatus(const QJsonObject &data, qint64 &out)
{
    const QJsonValue value = data.value("status");
    if (value.isUndefined()) {
        out = -1;
        return true;
    }
    if (!value.isDouble()) return false;
    out = value.toInteger(-2);
    return out == -1 || out == ORDER_RESERVED || out == ORDER_CHARGING
        || out == ORDER_TO_SETTLE || out == ORDER_SETTLED || out == ORDER_CANCELLED;
}

static bool validPage(const QJsonObject &data, qint64 &page, qint64 &size, qint64 &offset)
{
    if (!positiveInteger(data.value("page"), page)
        || !positiveInteger(data.value("size"), size)) {
        return false;
    }
    if (page - 1 > std::numeric_limits<qint64>::max() / size) return false;
    offset = (page - 1) * size;
    return true;
}

static bool optionalDateTime(const QJsonObject &data, const char *name,
                             QString &text, QDateTime &dateTime)
{
    const QJsonValue value = data.value(name);
    if (value.isUndefined()) {
        text = QStringLiteral("");
        dateTime = QDateTime();
        return true;
    }
    if (!value.isString()) return false;
    text = value.toString();
    if (text.isEmpty()) {
        dateTime = QDateTime();
        return true;
    }
    const QString format = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    dateTime = QDateTime::fromString(text, format);
    return dateTime.isValid() && dateTime.toString(format) == text;
}

static QString timeText(const QVariant &value)
{
    return value.isNull() ? QString() : value.toString();
}

static QJsonObject userOrderObject(const QSqlQuery &query)
{
    QJsonObject item;
    item["orderId"] = query.value("order_id").toLongLong();
    item["orderNo"] = query.value("order_no").toString();
    item["stationId"] = query.value("station_id").toLongLong();
    item["stationName"] = query.value("station_name").toString();
    item["pileId"] = query.value("pile_id").toLongLong();
    item["pileCode"] = query.value("pile_code").toString();
    item["status"] = query.value("status").toInt();
    item["price"] = query.value("price").toLongLong();
    item["kwh"] = query.value("kwh_x100").toLongLong() / 100.0;
    item["amount"] = query.value("amount").toLongLong();
    item["reserveTime"] = timeText(query.value("reserve_time"));
    item["startTime"] = timeText(query.value("start_time"));
    item["endTime"] = timeText(query.value("end_time"));
    item["settleTime"] = timeText(query.value("settle_time"));
    return item;
}

static QJsonObject adminOrderObject(const QSqlQuery &query)
{
    QJsonObject item = userOrderObject(query);
    item["userId"] = query.value("user_id").toLongLong();
    item["phone"] = query.value("phone").toString();
    item["nickname"] = query.value("nickname").toString();
    return item;
}

static int handleUnfinishedOrder(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询未完成订单获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.order_id, o.order_no, o.station_id, s.name AS station_name,"
        " o.pile_id, p.pile_code, o.status, o.price, o.kwh_x100, o.amount,"
        " o.reserve_time, o.start_time, o.end_time, o.settle_time"
        " FROM t_order o"
        " JOIN t_station s ON s.station_id = o.station_id"
        " JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.user_id = ? AND o.status IN (?, ?, ?)"
        " ORDER BY o.order_id DESC LIMIT 2"));
    query.addBindValue(req.session.id);
    query.addBindValue(ORDER_RESERVED);
    query.addBindValue(ORDER_CHARGING);
    query.addBindValue(ORDER_TO_SETTLE);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询用户未完成订单失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        out["hasUnfinished"] = false;
        out["order"] = QJsonObject();
        return ERR_OK;
    }

    const QJsonObject order = userOrderObject(query);
    if (query.next()) {
        LOG_W(QStringLiteral("用户存在多条未完成订单: userId=%1，返回最新 orderId=%2")
                  .arg(req.session.id).arg(order.value("orderId").toInteger()));
    }
    out["hasUnfinished"] = true;
    out["order"] = order;
    return ERR_OK;
}

static int handleUserOrderList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    qint64 offset = 0;
    qint64 status = -1;
    if (!validPage(req.data, page, size, offset)
        || !optionalOrderStatus(req.data, status)) {
        return ERR_PARAM;
    }

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询用户订单列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery count(db);
    count.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM t_order o"
        " WHERE o.user_id = ? AND (? = -1 OR o.status = ?)"));
    count.addBindValue(req.session.id);
    count.addBindValue(status);
    count.addBindValue(status);
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询用户订单总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取用户订单总数失败"));
        return ERR_INTERNAL;
    }
    const qint64 total = count.value(0).toLongLong();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.order_id, o.order_no, o.station_id, s.name AS station_name,"
        " o.pile_id, p.pile_code, o.status, o.price, o.kwh_x100, o.amount,"
        " o.reserve_time, o.start_time, o.end_time, o.settle_time"
        " FROM t_order o"
        " JOIN t_station s ON s.station_id = o.station_id"
        " JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.user_id = ? AND (? = -1 OR o.status = ?)"
        " ORDER BY o.reserve_time DESC, o.order_id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(req.session.id);
    query.addBindValue(status);
    query.addBindValue(status);
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询用户订单列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) list.append(userOrderObject(query));
    out["total"] = total;
    out["list"] = list;
    return ERR_OK;
}

static int handleAdminOrderList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    qint64 offset = 0;
    qint64 status = -1;
    QString dateFrom;
    QString dateTo;
    QDateTime from;
    QDateTime to;
    if (!validPage(req.data, page, size, offset)
        || !optionalOrderStatus(req.data, status)
        || !optionalDateTime(req.data, "dateFrom", dateFrom, from)
        || !optionalDateTime(req.data, "dateTo", dateTo, to)
        || (from.isValid() && to.isValid() && from > to)) {
        return ERR_PARAM;
    }

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询管理端订单列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery count(db);
    count.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM t_order o"
        " WHERE (? = -1 OR o.status = ?)"
        " AND (? = '' OR o.reserve_time >= ?)"
        " AND (? = '' OR o.reserve_time <= ?)"));
    count.addBindValue(status);
    count.addBindValue(status);
    count.addBindValue(dateFrom);
    count.addBindValue(dateFrom);
    count.addBindValue(dateTo);
    count.addBindValue(dateTo);
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询管理端订单总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取管理端订单总数失败"));
        return ERR_INTERNAL;
    }
    const qint64 total = count.value(0).toLongLong();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.order_id, o.order_no, o.user_id, u.phone, u.nickname,"
        " o.station_id, s.name AS station_name, o.pile_id, p.pile_code,"
        " o.status, o.price, o.kwh_x100, o.amount, o.reserve_time,"
        " o.start_time, o.end_time, o.settle_time"
        " FROM t_order o"
        " JOIN t_user u ON u.user_id = o.user_id"
        " JOIN t_station s ON s.station_id = o.station_id"
        " JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE (? = -1 OR o.status = ?)"
        " AND (? = '' OR o.reserve_time >= ?)"
        " AND (? = '' OR o.reserve_time <= ?)"
        " ORDER BY o.reserve_time DESC, o.order_id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(status);
    query.addBindValue(status);
    query.addBindValue(dateFrom);
    query.addBindValue(dateFrom);
    query.addBindValue(dateTo);
    query.addBindValue(dateTo);
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询管理端订单列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) list.append(adminOrderObject(query));
    out["total"] = total;
    out["list"] = list;
    return ERR_OK;
}

void registerOrderService()
{
    Dispatcher::instance().registerHandler(CMD_ORDER_UNFINISHED, handleUnfinishedOrder);
    Dispatcher::instance().registerHandler(CMD_ORDER_LIST, handleUserOrderList);
    Dispatcher::instance().registerHandler(CMD_ADMIN_ORDER_LIST, handleAdminOrderList);
    LOG_I(QStringLiteral("订单查询服务已注册: 1201 未完成订单 / 1207 我的订单 / 2304 管理端订单列表"));
}

} // namespace ecp
