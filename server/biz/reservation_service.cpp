// -----------------------------------------------------------------------------
//  server/biz/reservation_service.cpp  —  预约服务　归属 L2
//
//  [说明书] 1.4 预约充电与取消预约。
// -----------------------------------------------------------------------------
#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>
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

static void rollback(QSqlDatabase &db)
{
    if (!db.rollback())
        LOG_E(QStringLiteral("预约事务回滚失败: %1").arg(db.lastError().text()));
}

static QString makeOrderNo()
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"));
    const quint32 suffix = QRandomGenerator::global()->bounded(10000U);
    return QStringLiteral("ORD%1%2").arg(timestamp).arg(suffix, 4, 10, QLatin1Char('0'));
}

static bool isOrderNoConflict(const QSqlError &error)
{
    return error.text().contains(QStringLiteral("UNIQUE constraint failed: t_order.order_no"),
                                 Qt::CaseInsensitive);
}

static int handleReserve(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    qint64 pileId = 0;
    if (!positiveInteger(req.data.value("pileId"), pileId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("预约获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启预约立即事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery user(db);
    user.prepare(QStringLiteral("SELECT status FROM t_user WHERE user_id = ?"));
    user.addBindValue(req.session.id);
    if (!user.exec()) {
        LOG_E(QStringLiteral("预约查询用户失败: %1").arg(user.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!user.next()) {
        rollback(db);
        return ERR_USER_NOT_FOUND;
    }
    if (user.value("status").toInt() == USER_FROZEN) {
        rollback(db);
        return ERR_USER_FROZEN;
    }

    QSqlQuery pile(db);
    pile.prepare(QStringLiteral(
        "SELECT p.station_id, p.status, p.online, s.price"
        " FROM t_pile p JOIN t_station s ON s.station_id = p.station_id"
        " WHERE p.pile_id = ?"));
    pile.addBindValue(pileId);
    if (!pile.exec()) {
        LOG_E(QStringLiteral("预约查询电桩失败: %1").arg(pile.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!pile.next()) {
        rollback(db);
        return ERR_PILE_NOT_FOUND;
    }
    const int pileStatus = pile.value("status").toInt();
    if (pileStatus == PILE_FAULT) {
        rollback(db);
        return ERR_PILE_FAULT;
    }
    if (pileStatus != PILE_IDLE) {
        rollback(db);
        return ERR_PILE_BUSY;
    }
    if (pile.value("online").toInt() != 1) {
        rollback(db);
        return ERR_PILE_OFFLINE;
    }
    const qint64 stationId = pile.value("station_id").toLongLong();
    const qint64 price = pile.value("price").toLongLong();

    QSqlQuery userOrder(db);
    userOrder.prepare(QStringLiteral(
        "SELECT 1 FROM t_order WHERE user_id = ? AND status IN (?, ?, ?) LIMIT 1"));
    userOrder.addBindValue(req.session.id);
    userOrder.addBindValue(ORDER_RESERVED);
    userOrder.addBindValue(ORDER_CHARGING);
    userOrder.addBindValue(ORDER_TO_SETTLE);
    if (!userOrder.exec()) {
        LOG_E(QStringLiteral("预约查询用户未完成订单失败: %1").arg(userOrder.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (userOrder.next()) {
        rollback(db);
        return ERR_ORDER_UNFINISHED;
    }

    QSqlQuery pileOrder(db);
    pileOrder.prepare(QStringLiteral(
        "SELECT 1 FROM t_order WHERE pile_id = ? AND status IN (?, ?, ?) LIMIT 1"));
    pileOrder.addBindValue(pileId);
    pileOrder.addBindValue(ORDER_RESERVED);
    pileOrder.addBindValue(ORDER_CHARGING);
    pileOrder.addBindValue(ORDER_TO_SETTLE);
    if (!pileOrder.exec()) {
        LOG_E(QStringLiteral("预约查询电桩占用失败: %1").arg(pileOrder.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (pileOrder.next()) {
        rollback(db);
        return ERR_PILE_BUSY;
    }

    const QString reserveTime = nowStr();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO t_order(order_no, user_id, pile_id, station_id, status, price,"
        " kwh_x100, amount, reserve_time) VALUES(?, ?, ?, ?, ?, ?, 0, 0, ?)"));
    constexpr int maxOrderNoAttempts = 5;
    bool inserted = false;
    for (int attempt = 0; attempt < maxOrderNoAttempts; ++attempt) {
        insert.bindValue(0, makeOrderNo());
        insert.bindValue(1, req.session.id);
        insert.bindValue(2, pileId);
        insert.bindValue(3, stationId);
        insert.bindValue(4, ORDER_RESERVED);
        insert.bindValue(5, price);
        insert.bindValue(6, reserveTime);
        if (insert.exec()) {
            inserted = true;
            break;
        }
        if (!isOrderNoConflict(insert.lastError())) {
            LOG_E(QStringLiteral("创建预约订单失败: %1").arg(insert.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
        LOG_W(QStringLiteral("预约订单号冲突，第 %1 次重试").arg(attempt + 1));
    }
    if (!inserted) {
        LOG_E(QStringLiteral("预约订单号连续 %1 次冲突").arg(maxOrderNoAttempts));
        rollback(db);
        return ERR_INTERNAL;
    }

    const qint64 orderId = insert.lastInsertId().toLongLong();
    if (orderId <= 0) {
        LOG_E(QStringLiteral("预约成功插入但未取得有效 orderId: %1").arg(orderId));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!db.commit()) {
        LOG_E(QStringLiteral("提交预约事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    out["orderId"] = orderId;
    LOG_I(QStringLiteral("用户预约电桩成功: userId=%1 pileId=%2 orderId=%3")
              .arg(req.session.id).arg(pileId).arg(orderId));
    return ERR_OK;
}

static int handleCancel(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("取消预约获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!db.transaction()) {
        LOG_E(QStringLiteral("开启取消预约事务失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery order(db);
    order.prepare(QStringLiteral(
        "SELECT status FROM t_order WHERE order_id = ? AND user_id = ?"));
    order.addBindValue(orderId);
    order.addBindValue(req.session.id);
    if (!order.exec()) {
        LOG_E(QStringLiteral("取消预约查询订单失败: %1").arg(order.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!order.next()) {
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    if (order.value("status").toInt() != ORDER_RESERVED) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE t_order SET status = ?"
        " WHERE order_id = ? AND user_id = ? AND status = ?"));
    update.addBindValue(ORDER_CANCELLED);
    update.addBindValue(orderId);
    update.addBindValue(req.session.id);
    update.addBindValue(ORDER_RESERVED);
    if (!update.exec()) {
        LOG_E(QStringLiteral("取消预约更新订单失败: %1").arg(update.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (update.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }
    if (!db.commit()) {
        LOG_E(QStringLiteral("提交取消预约事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    LOG_I(QStringLiteral("用户取消预约成功: userId=%1 orderId=%2")
              .arg(req.session.id).arg(orderId));
    return ERR_OK;
}

void registerReservationService()
{
    Dispatcher::instance().registerHandler(CMD_ORDER_RESERVE, handleReserve);
    Dispatcher::instance().registerHandler(CMD_ORDER_CANCEL, handleCancel);
    LOG_I(QStringLiteral("预约服务已注册: 1202 预约电桩 / 1206 取消预约"));
}

} // namespace ecp
