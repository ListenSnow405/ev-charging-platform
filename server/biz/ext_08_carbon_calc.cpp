// -----------------------------------------------------------------------------
//  server/biz/ext_08_carbon_calc.cpp  —  扩展模块 08 碳排放计算核心（纯函数实现）
//  归属 L5。设计与口径见同名头文件与 docs/expand/08-实现规划.md 第 2 节。
// -----------------------------------------------------------------------------
#include "ext_08_carbon_calc.h"

#include <QStringList>
#include <algorithm>

namespace ecp {
namespace carbon {

static const char *TIME_FMT = "yyyy-MM-dd HH:mm:ss";

QString qualityName(Quality q)
{
    switch (q) {
    case QUALITY_METER_INTERVAL:  return QStringLiteral("METER_INTERVAL");
    case QUALITY_UNIFORM_ESTIMATE:return QStringLiteral("UNIFORM_ESTIMATE");
    default:                      return QStringLiteral("UNAVAILABLE");
    }
}

// ---- 时段表解析 --------------------------------------------------------------

// "HH:MM" → 自当日 0 点起的秒数；非法返回 -1。"24:00" 合法，仅可作为区间右端。
static int parseClock(const QString &text)
{
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (parts.size() != 2) return -1;
    bool okH = false, okM = false;
    const int h = parts[0].toInt(&okH);
    const int m = parts[1].toInt(&okM);
    if (!okH || !okM || h < 0 || h > 24 || m < 0 || m > 59) return -1;
    const int sec = h * 3600 + m * 60;
    return (sec > 86400) ? -1 : sec;
}

// 解析 "HH:MM-HH:MM,HH:MM-HH:MM"。右端小于等于左端视为跨午夜，就地拆成两片，
// 于是运算层看到的每一片都满足 0 <= from < to <= 86400，不必再处理回绕。
static bool parseRanges(const QString &text, QVector<TimeSlice> *out, QString *err)
{
    const QStringList items = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (items.isEmpty()) { *err = QStringLiteral("时段配置为空: %1").arg(text); return false; }

    for (const QString &item : items) {
        const QStringList ab = item.trimmed().split(QLatin1Char('-'));
        if (ab.size() != 2) { *err = QStringLiteral("时段格式应为 HH:MM-HH:MM: %1").arg(item); return false; }
        const int a = parseClock(ab[0]);
        const int b = parseClock(ab[1]);
        if (a < 0 || b < 0) { *err = QStringLiteral("时段时刻非法: %1").arg(item); return false; }
        if (a == b)         { *err = QStringLiteral("时段长度为零: %1").arg(item); return false; }
        if (a < b) {
            out->append(TimeSlice{a, b});
        } else {                            // 跨午夜，如 23:00-07:00
            if (a < 86400) out->append(TimeSlice{a, 86400});
            if (b > 0)     out->append(TimeSlice{0, b});
        }
    }
    return true;
}

// 任意两片是否重叠（同一集合内、或峰与谷之间）
static bool slicesOverlap(const QVector<TimeSlice> &a, const QVector<TimeSlice> &b)
{
    for (const TimeSlice &x : a)
        for (const TimeSlice &y : b)
            if (x.fromSec < y.toSec && y.fromSec < x.toSec) return true;
    return false;
}

static bool selfOverlap(const QVector<TimeSlice> &v)
{
    for (int i = 0; i < v.size(); ++i)
        for (int j = i + 1; j < v.size(); ++j)
            if (v[i].fromSec < v[j].toSec && v[j].fromSec < v[i].toSec) return true;
    return false;
}

TariffPlan parseTariffPlan(const QString &peakRanges, const QString &valleyRanges)
{
    TariffPlan plan;
    if (!parseRanges(peakRanges, &plan.peak, &plan.error))     return plan;
    if (!parseRanges(valleyRanges, &plan.valley, &plan.error)) return plan;

    if (selfOverlap(plan.peak))   { plan.error = QStringLiteral("峰段内部区间重叠");   return plan; }
    if (selfOverlap(plan.valley)) { plan.error = QStringLiteral("谷段内部区间重叠");   return plan; }
    // 峰谷重叠时平段会变成负数，宁可整表判非法，也不要悄悄算出一个错的构成
    if (slicesOverlap(plan.peak, plan.valley)) { plan.error = QStringLiteral("峰段与谷段区间重叠"); return plan; }

    plan.valid = true;
    return plan;
}

// ---- 分时分摊 ----------------------------------------------------------------

// [start, end) 与 slices 的重叠秒数。按自然日推进，逐日与每一片求交。
// 单时区、无夏令时，故可直接用 epoch 秒做交集运算（见头文件说明）。
static qint64 overlapSeconds(const QDateTime &start, const QDateTime &end,
                             const QVector<TimeSlice> &slices)
{
    const qint64 s = start.toSecsSinceEpoch();
    const qint64 e = end.toSecsSinceEpoch();
    qint64 sum = 0;

    for (QDate d = start.date(); d <= end.date(); d = d.addDays(1)) {
        const qint64 dayStart = QDateTime(d, QTime(0, 0, 0)).toSecsSinceEpoch();
        for (const TimeSlice &sl : slices) {
            const qint64 lo = std::max(s, dayStart + sl.fromSec);
            const qint64 hi = std::min(e, dayStart + sl.toSec);
            if (hi > lo) sum += hi - lo;
        }
    }
    return sum;
}

SplitResult splitOrder(const QDateTime &start, const QDateTime &end,
                       qint64 kwhX100, const TariffPlan &plan)
{
    SplitResult r;

    // 非法输入一律不分摊。08 文档第 7 节：非法值拒绝并记录，不进入聚合。
    // 注意 kwhX100 < 0 时连 unalloc 都不填 —— 把负电量塞进未分配项，
    // 只会让「四项之和 = 总量」这条守恒断言变成掩盖脏数据的帮凶。
    if (kwhX100 < 0 || kwhX100 > KWH_X100_MAX) return r;
    if (!plan.valid || !start.isValid() || !end.isValid()) { r.unalloc = kwhX100; return r; }

    const qint64 dur = start.secsTo(end);
    if (dur <= 0 || dur > SESSION_SEC_MAX) { r.unalloc = kwhX100; return r; }

    // 零电量是合法的：时间可用就照常判为可分摊，四项全零，不拉低完整度
    if (kwhX100 == 0) { r.quality = QUALITY_UNIFORM_ESTIMATE; return r; }

    const qint64 dPeak   = overlapSeconds(start, end, plan.peak);
    const qint64 dValley = overlapSeconds(start, end, plan.valley);
    const qint64 dFlat   = dur - dPeak - dValley;   // 平段 = 总时长扣掉峰谷
    if (dFlat < 0) { r.unalloc = kwhX100; return r; }   // 时段表若非法，前面已拦；此处兜底

    // 最大余数法：整除后把余下的单位按余数大小依次补给各段，
    // 保证 peak + flat + valley ≡ kwhX100，逐单成立、聚合后自然也成立。
    // 并列时固定顺序 峰 > 平 > 谷，结果才可复现（否则同一份数据两次跑出两个答案）。
    struct Part { qint64 dur; qint64 raw; qint64 rem; int order; };
    Part parts[3] = { {dPeak, 0, 0, 0}, {dFlat, 0, 0, 1}, {dValley, 0, 0, 2} };

    qint64 assigned = 0;
    for (Part &p : parts) {
        const qint64 num = kwhX100 * p.dur;      // ≤ 1e11 * 6.048e5 = 6e16，qint64 内
        p.raw = num / dur;
        p.rem = num - p.raw * dur;
        assigned += p.raw;
    }
    qint64 leftover = kwhX100 - assigned;        // 恒为 0..2
    Part *ordered[3] = { &parts[0], &parts[1], &parts[2] };
    std::stable_sort(ordered, ordered + 3, [](const Part *a, const Part *b) {
        if (a->rem != b->rem) return a->rem > b->rem;
        return a->order < b->order;
    });
    for (int i = 0; i < 3 && leftover > 0; ++i, --leftover) ordered[i]->raw += 1;

    r.peak    = parts[0].raw;
    r.flat    = parts[1].raw;
    r.valley  = parts[2].raw;
    r.unalloc = 0;
    // 只有订单总量与起止时间，没有表计区间 → 按时长估算，必须如实标注
    r.quality = QUALITY_UNIFORM_ESTIMATE;
    return r;
}

// ---- 排放与派生指标 ----------------------------------------------------------

qint64 emissionG(qint64 kwhX100, qint64 factorGPerKwh)
{
    if (kwhX100 < 0 || kwhX100 > KWH_X100_MAX) return -1;
    if (factorGPerKwh <= 0)                    return -1;
    // kwh_x100 / 100 = 度，再乘因子；+50 是四舍五入到整数克的整数写法
    return (kwhX100 * factorGPerKwh + 50) / 100;
}

qint64 intensityGPerKwh(qint64 emissionG, qint64 totalKwhX100)
{
    if (totalKwhX100 <= 0 || emissionG < 0) return NOT_APPLICABLE;
    return (emissionG * 100 + totalKwhX100 / 2) / totalKwhX100;
}

int completenessPct(qint64 totalKwhX100, qint64 unallocKwhX100)
{
    if (totalKwhX100 <= 0) return NOT_APPLICABLE;          // 「不适用」，不是 100
    const qint64 usable = totalKwhX100 - unallocKwhX100;
    if (usable <= 0) return 0;
    return static_cast<int>((usable * 100 + totalKwhX100 / 2) / totalKwhX100);
}

// ---- 因子 --------------------------------------------------------------------

bool isDayAlignedBoundary(const QString &timeText)
{
    const QDateTime t = QDateTime::fromString(timeText, QLatin1String(TIME_FMT));
    return t.isValid() && t.time() == QTime(0, 0, 0);
}

bool pickFactor(const QVector<Factor> &factors, const QString &atTime, Factor *out)
{
    const QDateTime at = QDateTime::fromString(atTime, QLatin1String(TIME_FMT));
    if (!at.isValid()) return false;

    for (const Factor &f : factors) {
        if (!f.enabled) continue;
        const QDateTime from = QDateTime::fromString(f.effectFrom, QLatin1String(TIME_FMT));
        if (!from.isValid() || at < from) continue;              // 左闭
        if (!f.effectTo.isEmpty()) {
            const QDateTime to = QDateTime::fromString(f.effectTo, QLatin1String(TIME_FMT));
            if (!to.isValid() || at >= to) continue;             // 右开
        }
        if (out) *out = f;
        return true;
    }
    return false;
}

bool factorOverlaps(const QVector<Factor> &existing, const Factor &incoming, int skipFactorId)
{
    const QDateTime nf = QDateTime::fromString(incoming.effectFrom, QLatin1String(TIME_FMT));
    if (!nf.isValid()) return false;                             // 格式非法由调用方按 ERR_PARAM 处理
    const QDateTime nt = incoming.effectTo.isEmpty()
                       ? QDateTime()
                       : QDateTime::fromString(incoming.effectTo, QLatin1String(TIME_FMT));

    for (const Factor &f : existing) {
        if (f.factorId == skipFactorId) continue;
        if (f.region != incoming.region) continue;               // 区间只在同区域内比较
        const QDateTime ef = QDateTime::fromString(f.effectFrom, QLatin1String(TIME_FMT));
        if (!ef.isValid()) continue;
        const QDateTime et = f.effectTo.isEmpty()
                           ? QDateTime()
                           : QDateTime::fromString(f.effectTo, QLatin1String(TIME_FMT));

        // 区间左闭右开，空的右端视为 +∞。[a,b) 与 [b,c) 相邻不算重叠。
        const bool leftOk  = et.isValid() && et <= nf;           // 既有整体在新区间之前
        const bool rightOk = nt.isValid() && nt <= ef;           // 既有整体在新区间之后
        if (!leftOk && !rightOk) return true;
    }
    return false;
}

} // namespace carbon
} // namespace ecp
