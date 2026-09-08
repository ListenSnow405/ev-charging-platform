// -----------------------------------------------------------------------------
//  server/biz/order_flow_service.cpp  —  用户充电流程服务 归属 L2
//
//  [说明书] 1.4 预约 -> 开始充电 -> 结束计费 -> 结算
// -----------------------------------------------------------------------------
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

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
        LOG_E(QStringLiteral("订单流程事务回滚失败: %1").arg(db.lastError().text()));
}

static qint64 chargeAmount(qint64 price, qint64 kwhX100)
{
    return (price * kwhX100 + 50) / 100;
}

static int handleOrderStart(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) return ERR_INTERNAL;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.status, o.pile_id, p.status AS pile_status, p.online"
        " FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.order_id = ? AND o.user_id = ?"));
    query.addBindValue(orderId);
    query.addBindValue(req.session.id);
    if (!query.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    if (query.value("status").toInt() != ORDER_RESERVED) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }
    if (query.value("pile_status").toInt() == PILE_FAULT) {
        rollback(db);
        return ERR_PILE_FAULT;
    }
    if (query.value("pile_status").toInt() == PILE_IN_USE) {
        rollback(db);
        return ERR_PILE_BUSY;
    }
    if (query.value("online").toInt() != 1) {
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    const qint64 pileId = query.value("pile_id").toLongLong();
    const QString now = nowStr();

    QSqlQuery updateOrder(db);
    updateOrder.prepare(QStringLiteral(
        "UPDATE t_order SET status = ?, start_time = ?, end_time = NULL, kwh_x100 = 0, amount = 0"
        " WHERE order_id = ? AND user_id = ? AND status = ?"));
    updateOrder.addBindValue(ORDER_CHARGING);
    updateOrder.addBindValue(now);
    updateOrder.addBindValue(orderId);
    updateOrder.addBindValue(req.session.id);
    updateOrder.addBindValue(ORDER_RESERVED);
    if (!updateOrder.exec() || updateOrder.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    QSqlQuery updatePile(db);
    updatePile.prepare(QStringLiteral(
        "UPDATE t_pile SET status = ?, last_heartbeat = ? WHERE pile_id = ?"));
    updatePile.addBindValue(PILE_IN_USE);
    updatePile.addBindValue(now);
    updatePile.addBindValue(pileId);
    if (!updatePile.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    out["startTime"] = now;
    return ERR_OK;
}

static int handleOrderStop(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) return ERR_INTERNAL;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.status, o.pile_id, o.price, o.start_time, p.power"
        " FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.order_id = ? AND o.user_id = ?"));
    query.addBindValue(orderId);
    query.addBindValue(req.session.id);
    if (!query.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    if (query.value("status").toInt() != ORDER_CHARGING) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    const QString startTime = query.value("start_time").toString();
    const QString now = nowStr();
    qint64 seconds = secondsBetween(startTime, now);
    if (seconds <= 0) seconds = 1;
    qint64 kwhX100 = static_cast<qint64>(query.value("power").toDouble() * seconds * 100.0 / 3600.0 + 0.5);
    if (kwhX100 <= 0) kwhX100 = 1;
    const qint64 amount = chargeAmount(query.value("price").toLongLong(), kwhX100);
    const qint64 pileId = query.value("pile_id").toLongLong();

    QSqlQuery updateOrder(db);
    updateOrder.prepare(QStringLiteral(
        "UPDATE t_order SET status = ?, end_time = ?, kwh_x100 = ?, amount = ?"
        " WHERE order_id = ? AND user_id = ? AND status = ?"));
    updateOrder.addBindValue(ORDER_TO_SETTLE);
    updateOrder.addBindValue(now);
    updateOrder.addBindValue(kwhX100);
    updateOrder.addBindValue(amount);
    updateOrder.addBindValue(orderId);
    updateOrder.addBindValue(req.session.id);
    updateOrder.addBindValue(ORDER_CHARGING);
    if (!updateOrder.exec() || updateOrder.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    QSqlQuery updatePile(db);
    updatePile.prepare(QStringLiteral(
        "UPDATE t_pile SET status = ?, charge_count = charge_count + 1,"
        " charge_duration = charge_duration + ?, last_heartbeat = ? WHERE pile_id = ?"));
    updatePile.addBindValue(PILE_IDLE);
    updatePile.addBindValue(seconds);
    updatePile.addBindValue(now);
    updatePile.addBindValue(pileId);
    if (!updatePile.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    out["endTime"] = now;
    out["kwh"] = kwhX100 / 100.0;
    out["amount"] = amount;
    return ERR_OK;
}

static int handleOrderSettle(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) return ERR_INTERNAL;

    QSqlQuery order(db);
    order.prepare(QStringLiteral(
        "SELECT status, amount FROM t_order WHERE order_id = ? AND user_id = ?"));
    order.addBindValue(orderId);
    order.addBindValue(req.session.id);
    if (!order.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!order.next()) {
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    if (order.value("status").toInt() != ORDER_TO_SETTLE) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    const qint64 amount = order.value("amount").toLongLong();
    if (amount <= 0) {
        rollback(db);
        return ERR_AMOUNT_INVALID;
    }

    QSqlQuery user(db);
    user.prepare(QStringLiteral("SELECT balance, status FROM t_user WHERE user_id = ?"));
    user.addBindValue(req.session.id);
    if (!user.exec()) {
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

    const qint64 balance = user.value("balance").toLongLong();
    if (balance < amount) {
        rollback(db);
        return ERR_BALANCE_NOT_ENOUGH;
    }

    const QString now = nowStr();
    const qint64 balanceAfter = balance - amount;

    QSqlQuery updateUser(db);
    updateUser.prepare(QStringLiteral(
        "UPDATE t_user SET balance = ?, update_time = ? WHERE user_id = ?"));
    updateUser.addBindValue(balanceAfter);
    updateUser.addBindValue(now);
    updateUser.addBindValue(req.session.id);
    if (!updateUser.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    QSqlQuery insertTx(db);
    insertTx.prepare(QStringLiteral(
        "INSERT INTO t_wallet_tx(user_id, type, amount, balance_after, order_id, remark, create_time)"
        " VALUES(?, 1, ?, ?, ?, ?, ?)"));
    insertTx.addBindValue(req.session.id);
    insertTx.addBindValue(amount);
    insertTx.addBindValue(balanceAfter);
    insertTx.addBindValue(orderId);
    insertTx.addBindValue(QStringLiteral("充电扣费"));
    insertTx.addBindValue(now);
    if (!insertTx.exec()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    QSqlQuery updateOrder(db);
    updateOrder.prepare(QStringLiteral(
        "UPDATE t_order SET status = ?, settle_time = ?"
        " WHERE order_id = ? AND user_id = ? AND status = ?"));
    updateOrder.addBindValue(ORDER_SETTLED);
    updateOrder.addBindValue(now);
    updateOrder.addBindValue(orderId);
    updateOrder.addBindValue(req.session.id);
    updateOrder.addBindValue(ORDER_TO_SETTLE);
    if (!updateOrder.exec() || updateOrder.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    if (!db.commit()) {
        rollback(db);
        return ERR_INTERNAL;
    }

    out["amount"] = amount;
    out["balance"] = balanceAfter;
    out["settleTime"] = now;
    return ERR_OK;
}

void registerOrderFlowService()
{
    Dispatcher::instance().registerHandler(CMD_ORDER_START, handleOrderStart);
    Dispatcher::instance().registerHandler(CMD_ORDER_STOP, handleOrderStop);
    Dispatcher::instance().registerHandler(CMD_ORDER_SETTLE, handleOrderSettle);
    LOG_I(QStringLiteral("订单流程服务已注册: 1203 开始充电 / 1204 结束计费 / 1205 结算"));
}

} // namespace ecp
