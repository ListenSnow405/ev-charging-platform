#pragma once
// =============================================================================
//  common/logger.h  —  统一日志宏
//
//  冻结契约 · 属主 L1，其他人只读。
//  [本组自定] 日志格式。CLAUDE.md 第 7 节：禁止 qDebug() 与 printf 混用。
//
//  格式：2026-09-02 14:07:59 [I] [线程id] file.cpp:42 消息
//  统一格式的意义：联调周要靠两端日志对照命令字与时序，格式不一致就对不上。
// =============================================================================

#include <QDebug>
#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <pthread.h>

namespace ecp {

inline QString logPrefix(const char *level, const char *file, int line)
{
    return QStringLiteral("%1 [%2] [%3] %4:%5")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
        .arg(QString::fromLatin1(level))
        .arg(static_cast<quint64>(pthread_self()), 0, 16)
        .arg(QFileInfo(QString::fromLatin1(file)).fileName())
        .arg(line);
}

} // namespace ecp

#define LOG_I(msg) qInfo().noquote()     << ecp::logPrefix("I", __FILE__, __LINE__) << (msg)
#define LOG_W(msg) qWarning().noquote()  << ecp::logPrefix("W", __FILE__, __LINE__) << (msg)
#define LOG_E(msg) qCritical().noquote() << ecp::logPrefix("E", __FILE__, __LINE__) << (msg)
