// -----------------------------------------------------------------------------
//  server/biz/user_management_service.cpp  —  用户管理服务　归属 L2
//
//  [说明书] 1.4 管理端支持用户列表、手机号模糊搜索、冻结与解冻用户。
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
        LOG_E(QStringLiteral("用户管理事务回滚失败: %1").arg(db.lastError().text()));
}

static int handleUserList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 page = 0;
    qint64 size = 0;
    if (!positiveInteger(req.data.value("page"), page)
        || !positiveInteger(req.data.value("size"), size)) {
        return ERR_PARAM;
    }

    const QJsonValue phoneLikeValue = req.data.value("phoneLike");
    if (!phoneLikeValue.isString()) return ERR_PARAM;
    const QString phonePattern = QStringLiteral("%") + phoneLikeValue.toString()
                                 + QStringLiteral("%");

    if (page - 1 > std::numeric_limits<qint64>::max() / size) return ERR_PARAM;
    const qint64 offset = (page - 1) * size;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("查询用户列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery count(db);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM t_user WHERE phone LIKE ?"));
    count.addBindValue(phonePattern);
    if (!count.exec()) {
        LOG_E(QStringLiteral("查询用户总数失败: %1").arg(count.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!count.next()) {
        LOG_E(QStringLiteral("读取用户总数失败"));
        return ERR_INTERNAL;
    }
    const qint64 total = count.value(0).toLongLong();

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT user_id, phone, nickname, balance, create_time, status"
        " FROM t_user WHERE phone LIKE ?"
        " ORDER BY create_time DESC, user_id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(phonePattern);
    query.addBindValue(size);
    query.addBindValue(offset);
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询用户列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["userId"]     = query.value("user_id").toLongLong();
        item["phone"]      = query.value("phone").toString();
        item["nickname"]   = query.value("nickname").toString();
        item["balance"]    = query.value("balance").toLongLong();
        item["createTime"] = query.value("create_time").toString();
        item["status"]     = query.value("status").toInt();
        list.append(item);
    }

    out["total"] = total;
    out["list"] = list;
    return ERR_OK;
}

static int handleUserStatus(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);

    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    qint64 userId = 0;
    if (!positiveInteger(req.data.value("userId"), userId)) return ERR_PARAM;

    const QJsonValue statusValue = req.data.value("status");
    if (!statusValue.isDouble()) return ERR_PARAM;
    const qint64 status = statusValue.toInteger(-1);
    if (status != USER_NORMAL && status != USER_FROZEN) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("更新用户状态获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!db.transaction()) {
        LOG_E(QStringLiteral("开启用户状态事务失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery exists(db);
    exists.prepare(QStringLiteral("SELECT 1 FROM t_user WHERE user_id = ?"));
    exists.addBindValue(userId);
    if (!exists.exec()) {
        LOG_E(QStringLiteral("确认用户存在失败: %1").arg(exists.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (!exists.next()) {
        rollback(db);
        return ERR_USER_NOT_FOUND;
    }

    const QString now = nowStr();
    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE t_user SET status = ?, update_time = ? WHERE user_id = ?"));
    update.addBindValue(status);
    update.addBindValue(now);
    update.addBindValue(userId);
    if (!update.exec()) {
        LOG_E(QStringLiteral("更新用户状态失败: %1").arg(update.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }
    if (update.numRowsAffected() == 0) {
        LOG_E(QStringLiteral("更新用户状态时用户不存在: userId=%1").arg(userId));
        rollback(db);
        return ERR_USER_NOT_FOUND;
    }

    const QString action = status == USER_FROZEN
                               ? QStringLiteral("FREEZE_USER")
                               : QStringLiteral("UNFREEZE_USER");
    QSqlQuery log(db);
    log.prepare(QStringLiteral(
        "INSERT INTO t_admin_oplog(admin_id, action, target, create_time) VALUES(?, ?, ?, ?)"));
    log.addBindValue(req.session.id);
    log.addBindValue(action);
    log.addBindValue(QString::number(userId));
    log.addBindValue(now);
    if (!log.exec()) {
        LOG_E(QStringLiteral("写入用户状态操作日志失败: %1").arg(log.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交用户状态事务失败: %1").arg(db.lastError().text()));
        rollback(db);
        return ERR_INTERNAL;
    }

    LOG_I(QStringLiteral("管理员操作用户状态: adminId=%1 action=%2 userId=%3")
              .arg(req.session.id).arg(action).arg(userId));
    return ERR_OK;
}

void registerUserManagementService()
{
    Dispatcher::instance().registerHandler(CMD_ADMIN_USER_LIST,   handleUserList);
    Dispatcher::instance().registerHandler(CMD_ADMIN_USER_STATUS, handleUserStatus);
    LOG_I(QStringLiteral("用户管理服务已注册: 2201 用户列表 / 2202 冻结解冻"));
}

} // namespace ecp
