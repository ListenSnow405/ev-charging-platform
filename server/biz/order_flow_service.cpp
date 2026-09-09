// -----------------------------------------------------------------------------
//  server/biz/order_flow_service.cpp  —  用户充电流程服务 归属 L2
//
//  [说明书] 1.4 预约 -> 开始充电 -> 结束计费 -> 结算
// -----------------------------------------------------------------------------
#include <cmath>
#include <limits>

#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include "protocol.h"
#include "frame.h"
#include "logger.h"
#include "time_util.h"
#include "net/dispatcher.h"
#include "net/device_registry.h"
#include "net/user_registry.h"
#include "dao/db.h"
#include "biz/order_flow_service.h"

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

void pushChargingProgress(const QString &pileCode, qint64 kwhX100)
{
    if (kwhX100 < 0) {
        LOG_E(QStringLiteral("实时推送累计电量为负数: pileCode=%1 kwhX100=%2")
                  .arg(pileCode).arg(kwhX100));
        return;
    }

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("实时推送获取数据库连接失败: %1").arg(db.lastError().text()));
        return;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.order_id, o.user_id, o.start_time, o.price"
        " FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE p.pile_code = ? AND o.status = ?"));
    query.addBindValue(pileCode);
    query.addBindValue(ORDER_CHARGING);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询实时推送充电订单失败: %1").arg(query.lastError().text()));
        return;
    }

    if (!query.next()) {
        LOG_I(QStringLiteral("电桩没有充电中订单，不推送实时数据: pileCode=%1").arg(pileCode));
        return;
    }

    const qint64 orderId = query.value("order_id").toLongLong();
    const int userId = query.value("user_id").toInt();
    const QString startTime = query.value("start_time").toString();
    bool priceOk = false;
    const qint64 price = query.value("price").toLongLong(&priceOk);
    if (query.next()) {
        LOG_E(QStringLiteral("电桩存在多个充电中订单，拒绝实时推送: pileCode=%1")
                  .arg(pileCode));
        return;
    }
    if (!priceOk || price <= 0
        || kwhX100 > std::numeric_limits<qint64>::max() / price) {
        LOG_E(QStringLiteral("实时推送价格或累计电量异常/乘法溢出: orderId=%1 price=%2 kwhX100=%3")
                  .arg(orderId).arg(price).arg(kwhX100));
        return;
    }

    const QDateTime start = fromStr(startTime);
    const QString now = nowStr();
    const qint64 duration = secondsBetween(startTime, now);
    if (!start.isValid() || toStr(start) != startTime || duration < 0) {
        LOG_E(QStringLiteral("实时推送开始时间非法或晚于当前时间: orderId=%1 startTime=%2 now=%3")
                  .arg(orderId).arg(startTime, now));
        return;
    }

    const qint64 amount = price * kwhX100 / 100;
    QJsonObject data;
    data["orderId"] = orderId;
    data["kwh"] = kwhX100 / 100.0;
    data["amount"] = amount;
    data["duration"] = duration;

    if (!pushToUser(userId, encodeFrame(buildPush(CMD_ORDER_PUSH, data)))) {
        LOG_W(QStringLiteral("实时充电数据推送失败，用户不在线: userId=%1 orderId=%2")
                  .arg(userId).arg(orderId));
        return;
    }
    LOG_I(QStringLiteral("实时充电数据推送成功: userId=%1 orderId=%2 pileCode=%3 kwhX100=%4")
              .arg(userId).arg(orderId).arg(pileCode).arg(kwhX100));
}

static int handleOrderStart(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("开始充电获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启开始充电立即事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.status, o.pile_id, p.status AS pile_status, p.pile_code"
        " FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.order_id = ? AND o.user_id = ?"));
    query.addBindValue(orderId);
    query.addBindValue(req.session.id);
    if (!query.exec()) {
        LOG_E(QStringLiteral("开始充电查询订单失败: %1").arg(query.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            LOG_E(QStringLiteral("开始充电读取订单失败: %1").arg(query.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
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
    if (query.value("pile_status").toInt() != PILE_IDLE) {
        rollback(db);
        return ERR_PILE_BUSY;
    }
    const QString pileCode = query.value("pile_code").toString();
    if (!DeviceRegistry::instance().isOnline(pileCode)) {
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    const qint64 pileId = query.value("pile_id").toLongLong();
    query.finish();
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
    if (!updateOrder.exec()) {
        LOG_E(QStringLiteral("开始充电更新订单失败: %1").arg(updateOrder.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (updateOrder.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    QSqlQuery updatePile(db);
    updatePile.prepare(QStringLiteral(
        "UPDATE t_pile SET status = ? WHERE pile_id = ? AND status = ?"));
    updatePile.addBindValue(PILE_IN_USE);
    updatePile.addBindValue(pileId);
    updatePile.addBindValue(PILE_IDLE);
    if (!updatePile.exec()) {
        LOG_E(QStringLiteral("开始充电更新电桩失败: %1").arg(updatePile.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (updatePile.numRowsAffected() != 1) {
        rollback(db);
        return ERR_PILE_BUSY;
    }

    // [说明书] 1.4 开始充电：数据库更新成功后、提交前由 L1 下发 9005。
    if (!startCharging(pileCode)) {
        LOG_W(QStringLiteral("开始充电指令入队失败: pileCode=%1").arg(pileCode));
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交开始充电事务失败: %1，尝试下发9006补偿: pileCode=%2")
                  .arg(db.lastError().text(), pileCode));
        rollback(db);
        if (!stopCharging(pileCode))
            LOG_E(QStringLiteral("开始充电提交失败后的9006补偿入队失败: pileCode=%1").arg(pileCode));
        return ERR_INTERNAL;
    }

    out["startTime"] = now;
    LOG_I(QStringLiteral("用户开始充电成功: userId=%1 orderId=%2 pileCode=%3")
              .arg(req.session.id).arg(orderId).arg(pileCode));
    return ERR_OK;
}

static int handleOrderStop(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    if (!positiveInteger(req.data.value("orderId"), orderId)) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("结束充电获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启结束充电立即事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT o.status, o.pile_id, o.price, o.start_time, p.pile_code, p.status AS pile_status"
        " FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id"
        " WHERE o.order_id = ? AND o.user_id = ?"));
    query.addBindValue(orderId);
    query.addBindValue(req.session.id);
    if (!query.exec()) {
        LOG_E(QStringLiteral("结束充电查询订单失败: %1").arg(query.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            LOG_E(QStringLiteral("结束充电读取订单失败: %1").arg(query.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    if (query.value("status").toInt() != ORDER_CHARGING) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    const QString pileCode = query.value("pile_code").toString();
    DeviceReport report;
    if (!DeviceRegistry::instance().isOnline(pileCode)
        || !DeviceRegistry::instance().lastReport(pileCode, report)) {
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    bool priceOk = false;
    const qint64 price = query.value("price").toLongLong(&priceOk);
    const qint64 kwhX100 = report.kwhX100;
    if (!priceOk || price <= 0 || kwhX100 < 0
        || kwhX100 > std::numeric_limits<qint64>::max() / price) {
        LOG_E(QStringLiteral("结束充电价格或累计电量异常/乘法溢出: orderId=%1 price=%2 kwhX100=%3")
                  .arg(orderId).arg(price).arg(kwhX100));
        rollback(db);
        return ERR_INTERNAL;
    }
    // [说明书] 1.4 计费；协议 v1.3：金额为整数分，不足一分向下取整。
    const qint64 amount = price * kwhX100 / 100;

    const QString startTime = query.value("start_time").toString();
    const QDateTime start = fromStr(startTime);
    const QString now = nowStr();
    const qint64 seconds = secondsBetween(startTime, now);
    if (!start.isValid() || toStr(start) != startTime || seconds < 0) {
        LOG_E(QStringLiteral("结束充电开始时间非法或晚于结束时间: orderId=%1 startTime=%2 endTime=%3")
                  .arg(orderId).arg(startTime, now));
        rollback(db);
        return ERR_INTERNAL;
    }
    const qint64 pileId = query.value("pile_id").toLongLong();
    const int pileStatus = query.value("pile_status").toInt();
    query.finish();

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
    if (!updateOrder.exec()) {
        LOG_E(QStringLiteral("结束充电更新订单失败: %1").arg(updateOrder.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (updateOrder.numRowsAffected() != 1) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    QSqlQuery updatePile(db);
    updatePile.prepare(QStringLiteral(
        "UPDATE t_pile SET status = ?, charge_count = charge_count + 1,"
        " charge_duration = charge_duration + ? WHERE pile_id = ?"));
    updatePile.addBindValue(pileStatus == PILE_FAULT ? PILE_FAULT : PILE_IDLE);
    updatePile.addBindValue(seconds);
    updatePile.addBindValue(pileId);
    if (!updatePile.exec()) {
        LOG_E(QStringLiteral("结束充电更新电桩失败: %1").arg(updatePile.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (updatePile.numRowsAffected() != 1) {
        LOG_E(QStringLiteral("结束充电更新电桩影响行数异常: pileId=%1 rows=%2")
                  .arg(pileId).arg(updatePile.numRowsAffected()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!stopCharging(pileCode)) {
        LOG_W(QStringLiteral("结束充电指令入队失败: pileCode=%1").arg(pileCode));
        rollback(db);
        return ERR_PILE_OFFLINE;
    }

    // 9006 与数据库无法跨进程原子提交；失败后不能发9005补偿，会清零设备电量。
    if (!db.commit()) {
        LOG_E(QStringLiteral("9006已入队但结束充电事务提交失败: %1，设备停止无法原子回退: pileCode=%2")
                  .arg(db.lastError().text(), pileCode));
        rollback(db);
        return ERR_INTERNAL;
    }

    out["endTime"] = now;
    out["kwh"] = kwhX100 / 100.0;
    out["amount"] = amount;
    LOG_I(QStringLiteral("用户结束充电成功: userId=%1 orderId=%2 pileCode=%3 kwhX100=%4 amount=%5")
              .arg(req.session.id).arg(orderId).arg(pileCode).arg(kwhX100).arg(amount));
    return ERR_OK;
}

static int handleOrderSettle(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;
    qint64 orderId = 0;
    const QJsonValue orderIdValue = req.data.value("orderId");
    if (!orderIdValue.isDouble()) return ERR_PARAM;
    const double orderIdNumber = orderIdValue.toDouble();
    if (!std::isfinite(orderIdNumber) || orderIdNumber <= 0
        || std::floor(orderIdNumber) != orderIdNumber
        || orderIdNumber > static_cast<double>(std::numeric_limits<qint64>::max())
        || !positiveInteger(orderIdValue, orderId)) {
        return ERR_PARAM;
    }

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("订单结算获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启订单结算立即事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery order(db);
    order.prepare(QStringLiteral(
        "SELECT status, amount FROM t_order WHERE order_id = ? AND user_id = ?"));
    order.addBindValue(orderId);
    order.addBindValue(req.session.id);
    if (!order.exec()) {
        LOG_E(QStringLiteral("查询待结算订单失败: %1").arg(order.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!order.next()) {
        rollback(db);
        return ERR_ORDER_NOT_FOUND;
    }
    const int orderStatus = order.value("status").toInt();
    if (orderStatus == ORDER_SETTLED) {
        rollback(db);
        return ERR_ORDER_SETTLED;
    }
    if (orderStatus != ORDER_TO_SETTLE) {
        rollback(db);
        return ERR_ORDER_STATUS;
    }

    bool amountOk = false;
    const qint64 amount = order.value("amount").toLongLong(&amountOk);
    if (!amountOk || amount < 0) {
        LOG_E(QStringLiteral("订单金额异常: orderId=%1 amount=%2")
                  .arg(orderId).arg(amount));
        rollback(db);
        return ERR_INTERNAL;
    }

    QSqlQuery user(db);
    user.prepare(QStringLiteral("SELECT balance, status FROM t_user WHERE user_id = ?"));
    user.addBindValue(req.session.id);
    if (!user.exec()) {
        LOG_E(QStringLiteral("查询结算用户失败: %1").arg(user.lastError().text()));
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

    bool balanceOk = false;
    const qint64 balance = user.value("balance").toLongLong(&balanceOk);
    if (!balanceOk || balance < 0) {
        LOG_E(QStringLiteral("用户余额异常: userId=%1 balance=%2")
                  .arg(req.session.id).arg(balance));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (balance < amount) {
        rollback(db);
        return ERR_BALANCE_NOT_ENOUGH;
    }

    const QString now = nowStr();
    const qint64 balanceAfter = balance - amount;

    if (amount > 0) {
        QSqlQuery updateUser(db);
        updateUser.prepare(QStringLiteral(
            "UPDATE t_user SET balance = ?, update_time = ?"
            " WHERE user_id = ? AND status = ? AND balance >= ?"));
        updateUser.addBindValue(balanceAfter);
        updateUser.addBindValue(now);
        updateUser.addBindValue(req.session.id);
        updateUser.addBindValue(USER_NORMAL);
        updateUser.addBindValue(amount);
        if (!updateUser.exec()) {
            LOG_E(QStringLiteral("更新结算用户余额失败: %1").arg(updateUser.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
        if (updateUser.numRowsAffected() != 1) {
            LOG_E(QStringLiteral("更新结算用户余额影响行数异常: userId=%1 rows=%2")
                      .arg(req.session.id).arg(updateUser.numRowsAffected()));
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
            LOG_E(QStringLiteral("写入充电扣费流水失败: %1").arg(insertTx.lastError().text()));
            rollback(db);
            return ERR_INTERNAL;
        }
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
    if (!updateOrder.exec()) {
        LOG_E(QStringLiteral("更新订单结算状态失败: %1").arg(updateOrder.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (updateOrder.numRowsAffected() != 1) {
        LOG_E(QStringLiteral("更新订单结算状态影响行数异常: orderId=%1 rows=%2")
                  .arg(orderId).arg(updateOrder.numRowsAffected()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交订单结算事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    out["amount"] = amount;
    out["balance"] = balanceAfter;
    LOG_I(QStringLiteral("订单结算成功: userId=%1 orderId=%2 amount=%3 balance=%4")
              .arg(req.session.id).arg(orderId).arg(amount).arg(balanceAfter));
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
