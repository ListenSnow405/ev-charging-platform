#pragma once
// =============================================================================
//  common/time_util.h  —  时间与金额格式
//
//  冻结契约 · 属主 L1，其他人只读。
//  [本组自定] 时间格式 'yyyy-MM-dd HH:mm:ss'，与 docs/db-schema.sql 一致
//  [本组自定] 金额一律整数「分」，仅显示层除 100（CLAUDE.md 硬性规则第 3 条）
// =============================================================================

#include <QDateTime>
#include <QString>

namespace ecp {

// 全项目唯一的时间字符串格式。数据库、协议、日志都用它。
inline QString TIME_FMT() { return QStringLiteral("yyyy-MM-dd HH:mm:ss"); }

inline QString nowStr()                        { return QDateTime::currentDateTime().toString(TIME_FMT()); }
inline QString toStr(const QDateTime &dt)      { return dt.toString(TIME_FMT()); }
inline QDateTime fromStr(const QString &s)     { return QDateTime::fromString(s, TIME_FMT()); }

// 两个时间字符串之间的秒数（用于累计充电时长）
inline qint64 secondsBetween(const QString &from, const QString &to)
{
    return fromStr(from).secsTo(fromStr(to));
}

// ---- 金额：内部一律 qint64「分」 ------------------------------------------------
// 仅在界面显示时调用，业务计算中禁止把金额转成浮点。
inline QString fenToYuan(qint64 fen)
{
    return QStringLiteral("%1.%2")
        .arg(fen / 100)
        .arg(qAbs(fen % 100), 2, 10, QLatin1Char('0'));
}

// 用户输入的元（字符串）→ 分。解析失败返回 -1，调用方回 ERR_AMOUNT_INVALID。
inline qint64 yuanToFen(const QString &yuan)
{
    bool ok = false;
    const double v = yuan.trimmed().toDouble(&ok);
    if (!ok || v < 0) return -1;
    return static_cast<qint64>(v * 100 + 0.5);   // 只在此处出现浮点，且立即取整
}

} // namespace ecp
