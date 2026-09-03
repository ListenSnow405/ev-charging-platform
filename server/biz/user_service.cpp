// -----------------------------------------------------------------------------
//  server/biz/user_service.cpp  —  用户服务　归属 L2
//
//  [说明书] 1.4 手机号免密登录：输入 11 位手机号，库中存在即登录；
//              不存在则自动创建新用户（默认昵称「用户」+ 手机号后 4 位），
//              首次登录即完成注册。登录后展示头像、昵称、钱包余额。
//
//  ⚠ 这是 L2 的样板实现，示范本模块必须遵守的写法：
//    ecp::threadDb() 取连接 · prepare+bindValue 参数绑定 · 金额整数分 · 返回 ErrCode
//    其余八个服务照此实现，清单见 server/biz/README.md
// -----------------------------------------------------------------------------
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

// 把一行用户数据填进响应
static void fillUser(QJsonObject &out, const QSqlQuery &q)
{
    out["userId"]   = q.value("user_id").toInt();
    out["phone"]    = q.value("phone").toString();
    out["nickname"] = q.value("nickname").toString();
    out["avatar"]   = q.value("avatar").toString();
    out["balance"]  = static_cast<double>(q.value("balance").toLongLong()); // 单位：分
    out["status"]   = q.value("status").toInt();
}

static int handleLogin(const Request &req, QJsonObject &out)
{
    const QString phone = req.data.value("phone").toString().trimmed();

    // [说明书] 1.4 11 位手机号
    if (phone.size() != 11) return ERR_PHONE_FORMAT;
    for (const QChar &c : phone)
        if (!c.isDigit()) return ERR_PHONE_FORMAT;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT user_id, phone, nickname, avatar, balance, status FROM t_user WHERE phone = ?"));
    q.addBindValue(phone);
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询用户失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }

    if (!q.next()) {
        // [说明书] 1.4 手机号不存在 → 自动创建新用户，首次登录即完成注册
        const QString nickname = QStringLiteral("用户") + phone.right(4);
        const QString now      = nowStr();

        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO t_user(phone, nickname, avatar, balance, status, create_time, update_time)"
            " VALUES(?, ?, '', 0, 0, ?, ?)"));
        ins.addBindValue(phone);
        ins.addBindValue(nickname);
        ins.addBindValue(now);
        ins.addBindValue(now);
        if (!ins.exec()) {
            LOG_E(QStringLiteral("自动注册失败: %1").arg(ins.lastError().text()));
            return ERR_INTERNAL;
        }
        LOG_I(QStringLiteral("新用户自动注册: %1 (%2)").arg(phone, nickname));

        q.exec();               // 重新查一次，拿到自增 user_id
        if (!q.next()) return ERR_INTERNAL;
    }

    // [说明书] 1.4 冻结账号不得登录（管理员风控）
    if (q.value("status").toInt() == USER_FROZEN) return ERR_USER_FROZEN;

    fillUser(out, q);
    out["token"] = SessionTable::instance().create(q.value("user_id").toInt(), ROLE_USER);
    LOG_I(QStringLiteral("用户登录成功: %1").arg(phone));
    return ERR_OK;
}

static int handleUserInfo(const Request &req, QJsonObject &out)
{
    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT user_id, phone, nickname, avatar, balance, status FROM t_user WHERE user_id = ?"));
    q.addBindValue(req.session.id);       // 来自 token，不信任客户端传的 id
    if (!q.exec())  return ERR_INTERNAL;
    if (!q.next())  return ERR_USER_NOT_FOUND;

    fillUser(out, q);
    return ERR_OK;
}

static int handleSetNickname(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);

    const QJsonValue value = req.data.value("nickname");
    if (!value.isString()) return ERR_PARAM;
    const QString nickname = value.toString().trimmed();
    if (nickname.isEmpty()) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE t_user SET nickname = ?, update_time = ? WHERE user_id = ?"));
    q.addBindValue(nickname);
    q.addBindValue(nowStr());
    q.addBindValue(req.session.id);       // 来自 token，不信任客户端传的 id
    if (!q.exec()) {
        LOG_E(QStringLiteral("修改用户昵称失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }
    if (q.numRowsAffected() == 0) return ERR_USER_NOT_FOUND;

    LOG_I(QStringLiteral("用户昵称修改成功: userId=%1").arg(req.session.id));
    return ERR_OK;
}

static int handleSetAvatar(const Request &req, QJsonObject &out)
{
    Q_UNUSED(out);

    const QJsonValue value = req.data.value("avatarPath");
    if (!value.isString()) return ERR_PARAM;
    const QString avatarPath = value.toString().trimmed();
    if (avatarPath.isEmpty()) return ERR_PARAM;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) return ERR_INTERNAL;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE t_user SET avatar = ?, update_time = ? WHERE user_id = ?"));
    q.addBindValue(avatarPath);
    q.addBindValue(nowStr());
    q.addBindValue(req.session.id);       // 来自 token，不信任客户端传的 id
    if (!q.exec()) {
        LOG_E(QStringLiteral("修改用户头像失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }
    if (q.numRowsAffected() == 0) return ERR_USER_NOT_FOUND;

    LOG_I(QStringLiteral("用户头像修改成功: userId=%1").arg(req.session.id));
    return ERR_OK;
}

void registerUserService()
{
    Dispatcher::instance().registerHandler(CMD_USER_LOGIN,        handleLogin, /*needAuth=*/false);
    Dispatcher::instance().registerHandler(CMD_USER_INFO,         handleUserInfo);
    Dispatcher::instance().registerHandler(CMD_USER_SET_NICKNAME, handleSetNickname);
    Dispatcher::instance().registerHandler(CMD_USER_SET_AVATAR,   handleSetAvatar);
    LOG_I(QStringLiteral("用户服务已注册: 1001 登录 / 1002 用户信息 / "
                         "1003 修改昵称 / 1004 修改头像"));
}

} // namespace ecp
