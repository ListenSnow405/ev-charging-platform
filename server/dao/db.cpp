#include "db.h"
#include "logger.h"

#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <pthread.h>

namespace ecp {

static QString g_dbPath = QStringLiteral("charging.db");

void    setDbPath(const QString &path) { g_dbPath = path; }
QString dbPath()                       { return g_dbPath; }

static QString connName()
{
    // 每线程一个连接名 —— 这是不跨线程共享连接的关键
    return QStringLiteral("ecp_conn_%1").arg(static_cast<quint64>(pthread_self()), 0, 16);
}

QSqlDatabase threadDb()
{
    const QString name = connName();
    if (QSqlDatabase::contains(name))
        return QSqlDatabase::database(name);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    db.setDatabaseName(g_dbPath);
    if (!db.open()) {
        LOG_E(QStringLiteral("打开数据库失败 [%1]: %2").arg(g_dbPath, db.lastError().text()));
        return db;
    }
    // 多线程写入时减少 database is locked
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    LOG_I(QStringLiteral("线程数据库连接已建立: %1").arg(name));
    return db;
}

void closeThreadDb()
{
    const QString name = connName();
    if (!QSqlDatabase::contains(name)) return;
    { QSqlDatabase::database(name).close(); }
    QSqlDatabase::removeDatabase(name);
}

} // namespace ecp
