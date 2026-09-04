// -----------------------------------------------------------------------------
//  server/biz/wallet_service.cpp  —  钱包服务　归属 L2
//
//  [说明书] 1.4 钱包充值（模拟支付），余额实时更新。
// -----------------------------------------------------------------------------
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
        LOG_E(QStringLiteral("钱包事务回滚失败: %1").arg(db.lastError().text()));
}

static int handleRecharge(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    const QJsonValue amountValue = req.data.value("amount");
    if (amountValue.isUndefined() || !amountValue.isDouble()) return ERR_PARAM;

    const qint64 amount = amountValue.toInteger(-1);
    if (amount <= 0) return ERR_AMOUNT_INVALID;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("钱包充值获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery cfg(db);
    cfg.prepare(QStringLiteral("SELECT cfg_value FROM t_sys_config WHERE cfg_key = ?"));
    cfg.addBindValue(QStringLiteral("recharge_max"));
    if (!cfg.exec()) {
        LOG_E(QStringLiteral("查询充值上限失败: %1").arg(cfg.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!cfg.next()) {
        LOG_E(QStringLiteral("系统配置缺少 recharge_max"));
        return ERR_INTERNAL;
    }

    bool maxOk = false;
    const qint64 rechargeMax = cfg.value("cfg_value").toString().toLongLong(&maxOk);
    if (!maxOk || rechargeMax <= 0) {
        LOG_E(QStringLiteral("系统配置 recharge_max 非法"));
        return ERR_INTERNAL;
    }
    if (amount > rechargeMax) return ERR_AMOUNT_INVALID;

    QSqlQuery begin(db);
    begin.prepare(QStringLiteral("BEGIN IMMEDIATE"));
    if (!begin.exec()) {
        LOG_E(QStringLiteral("开启钱包充值立即事务失败: %1").arg(begin.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery user(db);
    user.prepare(QStringLiteral("SELECT status FROM t_user WHERE user_id = ?"));
    user.addBindValue(req.session.id);
    if (!user.exec()) {
        LOG_E(QStringLiteral("钱包充值查询用户失败: %1").arg(user.lastError().text()));
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

    const QString now = nowStr();
    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE t_user SET balance = balance + ?, update_time = ? WHERE user_id = ?"));
    update.addBindValue(amount);
    update.addBindValue(now);
    update.addBindValue(req.session.id);
    if (!update.exec()) {
        LOG_E(QStringLiteral("更新用户余额失败: %1").arg(update.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (update.numRowsAffected() == 0) {
        rollback(db);
        return ERR_USER_NOT_FOUND;
    }

    QSqlQuery balanceQuery(db);
    balanceQuery.prepare(QStringLiteral("SELECT balance FROM t_user WHERE user_id = ?"));
    balanceQuery.addBindValue(req.session.id);
    if (!balanceQuery.exec()) {
        LOG_E(QStringLiteral("查询充值后余额失败: %1").arg(balanceQuery.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!balanceQuery.next()) {
        LOG_E(QStringLiteral("充值后用户记录不存在: userId=%1").arg(req.session.id));
        rollback(db);
        return ERR_USER_NOT_FOUND;
    }
    const qint64 balance = balanceQuery.value("balance").toLongLong();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO t_wallet_tx(user_id, type, amount, balance_after, order_id, remark, create_time)"
        " VALUES(?, 0, ?, ?, NULL, ?, ?)"));
    insert.addBindValue(req.session.id);
    insert.addBindValue(amount);
    insert.addBindValue(balance);
    insert.addBindValue(QStringLiteral("钱包充值"));
    insert.addBindValue(now);
    if (!insert.exec()) {
        LOG_E(QStringLiteral("写入充值流水失败: %1").arg(insert.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交钱包充值事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    out["balance"] = balance;
    LOG_I(QStringLiteral("钱包充值成功: userId=%1 amount=%2 balance=%3")
              .arg(req.session.id).arg(amount).arg(balance));
    return ERR_OK;
}

static int handleWalletTxList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_USER) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    if (!positiveInteger(req.data.value("page"), page)
        || !positiveInteger(req.data.value("size"), size)) {
        return ERR_PARAM;
    }
    if (page - 1 > std::numeric_limits<qint64>::max() / size) return ERR_PARAM;
    const qint64 offset = (page - 1) * size;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询钱包流水获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery count(db);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM t_wallet_tx WHERE user_id = ?"));
    count.addBindValue(req.session.id);
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询钱包流水总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取钱包流水总数失败"));
        return ERR_INTERNAL;
    }
    const qint64 total = count.value(0).toLongLong();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT tx_id, type, amount, balance_after, order_id, remark, create_time"
        " FROM t_wallet_tx WHERE user_id = ?"
        " ORDER BY create_time DESC, tx_id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(req.session.id);
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询钱包流水列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["txId"]         = query.value("tx_id").toLongLong();
        item["type"]         = query.value("type").toInt();
        item["amount"]       = query.value("amount").toLongLong();
        item["balanceAfter"] = query.value("balance_after").toLongLong();
        item["orderId"]      = query.value("order_id").isNull()
                                   ? QJsonValue(QJsonValue::Null)
                                   : QJsonValue(query.value("order_id").toLongLong());
        item["remark"]       = query.value("remark").toString();
        item["createTime"]   = query.value("create_time").toString();
        list.append(item);
    }

    out["total"] = total;
    out["list"] = list;
    return ERR_OK;
}

void registerWalletService()
{
    Dispatcher::instance().registerHandler(CMD_USER_RECHARGE,  handleRecharge);
    Dispatcher::instance().registerHandler(CMD_WALLET_TX_LIST, handleWalletTxList);
    LOG_I(QStringLiteral("钱包服务已注册: 1005 充值 / 1006 钱包流水"));
}

} // namespace ecp
