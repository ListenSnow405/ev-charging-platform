// -----------------------------------------------------------------------------
//  server/biz/admin_service.cpp  —  管理员服务　归属 L2
//
//  [说明书] 1.4 管理员输入账号/密码验证；账号密码存于数据库管理员表；
//              默认初始账号 admin / 123456；登录成功进入管理后台主界面。
//
//  [本组自定] 客户端发明文密码，服务端取 SHA-256 与库中摘要比对。
//             库里不存明文（docs/protocol.md 第 6 节）。实训环境不做 TLS，属已知取舍。
// -----------------------------------------------------------------------------
#include <QCryptographicHash>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "protocol.h"
#include "logger.h"
#include "time_util.h"
#include "net/dispatcher.h"
#include "net/session.h"
#include "dao/db.h"

namespace ecp {

static QString sha256Hex(const QString &plain)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(plain.toUtf8(), QCryptographicHash::Sha256).toHex());
}

static int handleAdminLogin(const Request &req, QJsonObject &out)
{
    const QString account  = req.data.value("account").toString().trimmed();
    const QString password = req.data.value("password").toString();
    if (account.isEmpty() || password.isEmpty()) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT admin_id, account, password, status FROM t_admin WHERE account = ?"));
    q.addBindValue(account);
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询管理员失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }
    // 账号不存在与密码错误返回同一个码，避免暴露哪些账号存在
    if (!q.next()) return ERR_ADMIN_AUTH;
    if (q.value("password").toString() != sha256Hex(password)) {
        LOG_W(QStringLiteral("管理员密码错误: %1").arg(account));
        return ERR_ADMIN_AUTH;
    }
    if (q.value("status").toInt() != 0) return ERR_NO_PERMISSION;

    const int adminId = q.value("admin_id").toInt();

    QSqlQuery upd(db);
    upd.prepare(QStringLiteral("UPDATE t_admin SET last_login = ? WHERE admin_id = ?"));
    upd.addBindValue(nowStr());
    upd.addBindValue(adminId);
    upd.exec();

    out["adminId"] = adminId;
    out["account"] = account;
    out["token"]   = SessionTable::instance().create(adminId, ROLE_ADMIN);
    LOG_I(QStringLiteral("管理员登录成功: %1").arg(account));
    return ERR_OK;
}

void registerAdminService()
{
    Dispatcher::instance().registerHandler(CMD_ADMIN_LOGIN, handleAdminLogin, /*needAuth=*/false);
    LOG_I(QStringLiteral("管理员服务已注册: 2001 登录"));
}

} // namespace ecp
