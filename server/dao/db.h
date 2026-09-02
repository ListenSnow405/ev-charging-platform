#pragma once
// -----------------------------------------------------------------------------
//  server/dao/db.h  —  SQLite 连接管理
//  归属 L2。 [说明书] 1.6 QSQLite
//
//  ⚠ CLAUDE.md 硬性规则第 2 条：
//    禁止跨线程共享同一个 QSqlDatabase 连接。
//    threadDb() 为每个 pthread 工作线程返回一个独立连接，连接名由线程 id 生成。
//    所有 DAO 一律通过 threadDb() 取连接，不要自己 addDatabase。
// -----------------------------------------------------------------------------
#include <QSqlDatabase>
#include <QString>

namespace ecp {

// 设置数据库文件路径（进程启动时调用一次）
void   setDbPath(const QString &path);
QString dbPath();

// 取当前线程的连接；首次调用会创建并 open。失败时返回的 db.isOpen()==false
QSqlDatabase threadDb();

// 关闭当前线程的连接（工作线程退出前调用，可选）
void closeThreadDb();

} // namespace ecp
