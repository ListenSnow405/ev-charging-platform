// -----------------------------------------------------------------------------
//  server/biz/ext_08_carbon_service.cpp  —  扩展模块 08「碳减排与能源报告」
//  归属 L5（模块 08 认领人）。接缝规则见 docs/expand/00 第 5.2 节：
//  本模块一律新建独立文件，不改他人既有 service；server/main.cpp 只加一行注册。
//
//  任务规格 docs/expand/08-碳减排与能源报告.md
//  落地方案 docs/expand/08-实现规划.md（口径已冻结，改动先改该文档）
//
//  ⚠ 只读业务表，只写 t_carbon_*。
//    本文件不得出现对 t_order / t_user / t_wallet_tx / t_pile 的
//    INSERT / UPDATE / DELETE —— 08 文档第 4.2 节不变量 1：
//    任何聚合、预览、导出或重算都不得修改订单、钱包、设备遥测、支付和正式营收。
//
//  ⚠ 所有算式在 ext_08_carbon_calc.{h,cpp}（纯函数，有手算夹具）。
//    本文件只负责取数、循环、落库 —— 不要在这里就地再写一遍算式，
//    那样夹具就管不到它了，两边迟早分叉。
//
//  实施进度：S1 底座 → S2 计算核心 → S3 聚合与查询
//            → S4 因子管理 → **S5 报告、导出与 STALE（本文件当前状态，命令字已全部落地）**
// -----------------------------------------------------------------------------
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "protocol.h"
#include "protocol_ext.h"
#include "error_code_ext.h"
#include "logger.h"
#include "net/dispatcher.h"
#include "dao/db.h"
#include "ext_08_carbon_calc.h"

namespace ecp {

using namespace ecp::carbon;

static const char *DATE_FMT = "yyyy-MM-dd";
static const char *DATETIME_FMT = "yyyy-MM-dd HH:mm:ss";

// 查询范围上限。8292 单量级下 366 天是秒级，但入参来自客户端，
// 不设上限就等于允许一个请求把整个线程池占住。
static constexpr int MAX_RANGE_DAYS = 366;

// 与 ext_08_carbon_calc.h 的 NOT_APPLICABLE 同值，导出时判「不适用」用
static constexpr int NOT_APPLICABLE_VALUE = -1;

// 页面与导出三处必须展示同一份字符串（实现规划第 2 节）
static QString disclaimerText()
{
    return QStringLiteral("课程项目估算，非认证碳数据，不可用于碳交易或监管申报");
}

static QString nowText()
{
    return QDateTime::currentDateTime().toString(QLatin1String(DATETIME_FMT));
}

// -----------------------------------------------------------------------------
//  取数小工具
// -----------------------------------------------------------------------------

// 读 t_sys_config 的峰谷时段。**运行期只读**（ml/CLAUDE.md 禁止 L5 运行期写该表），
// 种子由 docs/db-schema-ext-08.sql 的 INSERT OR IGNORE 落下。
static bool loadTariffPlan(QSqlDatabase &db, TariffPlan *plan)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT cfg_key, cfg_value FROM t_sys_config"
        " WHERE cfg_key IN ('carbon_peak_ranges', 'carbon_valley_ranges')"));
    if (!q.exec()) {
        LOG_E(QStringLiteral("读取峰谷时段配置失败: %1").arg(q.lastError().text()));
        return false;
    }
    QString peak, valley;
    while (q.next()) {
        if (q.value(0).toString() == QLatin1String("carbon_peak_ranges")) peak = q.value(1).toString();
        else                                                             valley = q.value(1).toString();
    }
    if (peak.isEmpty() || valley.isEmpty()) {
        LOG_E(QStringLiteral("峰谷时段配置缺失，请确认 db-schema-ext-08.sql 已执行"));
        return false;
    }
    *plan = parseTariffPlan(peak, valley);
    if (!plan->valid) {
        LOG_E(QStringLiteral("峰谷时段配置非法: %1（峰=%2 谷=%3）").arg(plan->error, peak, valley));
        return false;
    }
    return true;
}

// 读全部启用的因子，按 effect_from 倒序 —— pickFactor() 取首个命中者，
// 倒序即「最近生效的优先」，与左闭右开区间语义一致。
static bool loadFactors(QSqlDatabase &db, QVector<Factor> *factors)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT factor_id, region, version, effect_from, effect_to,"
        " factor_g_per_kwh, source, enabled"
        " FROM t_carbon_factor WHERE enabled = 1 ORDER BY effect_from DESC"));
    if (!q.exec()) {
        LOG_E(QStringLiteral("读取排放因子失败: %1").arg(q.lastError().text()));
        return false;
    }
    while (q.next()) {
        Factor f;
        f.factorId   = q.value(0).toInt();
        f.region     = q.value(1).toString();
        f.version    = q.value(2).toString();
        f.effectFrom = q.value(3).toString();
        f.effectTo   = q.value(4).isNull() ? QString() : q.value(4).toString();
        f.gPerKwh    = q.value(5).toLongLong();
        f.source     = q.value(6).toString();
        f.enabled    = q.value(7).toInt() != 0;
        factors->append(f);
    }
    return true;
}

// 某一天该用哪个因子。裁决 D6 保证因子边界对齐自然日，
// 所以「按日选因子」是精确的，不存在日内切片。
static bool factorForDate(const QVector<Factor> &factors, const QString &date, Factor *out)
{
    return pickFactor(factors, date + QStringLiteral(" 00:00:00"), out);
}

// -----------------------------------------------------------------------------
//  单日聚合
// -----------------------------------------------------------------------------

struct StationAcc {
    qint64 total = 0, peak = 0, flat = 0, valley = 0, unalloc = 0, estimated = 0;
    int    cnt = 0;
};

// 重算某一天的全部站点行。
//
// ⚠ 整日整体重算，不做「只重算某个站」：该日的行集必须自洽，
//   否则会出现「站 1 用新因子、站 2 还是旧数字」的半拉状态，对账时无从解释。
//
// 幂等做法：先按 (stat_date, factor_version, algo_version) 删净，再插入。
// 比单纯 INSERT OR REPLACE 多挡一种情况 —— 某站当日订单全部消失时，
// 它的旧行不会作为幽灵数字留在表里。删除范围带上两个版本号，
// 因此**其他因子版本/算法版本的历史结果原样保留**（不变量 6）。
static int aggregateDate(QSqlDatabase &db, const QString &date, const TariffPlan &plan,
                         const Factor &factor, const QString &cutoffTime, int *rowsWritten)
{
    QSqlQuery sel(db);
    sel.prepare(QStringLiteral(
        "SELECT station_id, kwh_x100, start_time, end_time"
        " FROM t_order WHERE status = ? AND date(settle_time) = ?"));
    sel.addBindValue(ORDER_SETTLED);          // 只计已结算，与 2301/2302 营收同源
    sel.addBindValue(date);
    if (!sel.exec()) {
        LOG_E(QStringLiteral("读取 %1 订单失败: %2").arg(date, sel.lastError().text()));
        return ERR_INTERNAL;
    }

    QHash<int, StationAcc> byStation;
    qint64 rawKwhSum = 0;                     // 原始 SUM(kwh_x100)，用于与分摊结果对账
    int    rejected  = 0;

    while (sel.next()) {
        const int    stationId = sel.value(0).toInt();
        const qint64 kwh       = sel.value(1).toLongLong();
        // start_time / end_time 可空（t_order 允许 NULL），空串解析出无效 QDateTime，
        // splitOrder() 会据此判 UNAVAILABLE —— 降级路径就在这里进入
        const QDateTime st = QDateTime::fromString(sel.value(2).toString(), QLatin1String(DATETIME_FMT));
        const QDateTime et = QDateTime::fromString(sel.value(3).toString(), QLatin1String(DATETIME_FMT));

        rawKwhSum += kwh;
        const SplitResult r = splitOrder(st, et, kwh, plan);

        StationAcc &a = byStation[stationId];
        a.cnt     += 1;
        a.total   += r.total();               // 注意不是 kwh：脏单被 splitOrder 整单拒绝时 total()=0
        a.peak    += r.peak;
        a.flat    += r.flat;
        a.valley  += r.valley;
        a.unalloc += r.unalloc;
        if (r.quality == QUALITY_UNIFORM_ESTIMATE) a.estimated += r.peak + r.flat + r.valley;
        if (r.total() != kwh) ++rejected;     // 电量为负或超上限，整单不计入任何项
    }

    if (rejected > 0) {
        // 不静默：分摊总量与 SUM(kwh_x100) 对不上时，必须能从日志查到是哪天、差多少
        LOG_W(QStringLiteral("%1 有 %2 笔订单电量非法被拒（原始合计 %3），"
                             "该日 total_kwh_x100 将小于 SUM(kwh_x100)")
                  .arg(date).arg(rejected).arg(rawKwhSum));
    }

    if (!db.transaction()) {
        LOG_E(QStringLiteral("开启事务失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery del(db);
    del.prepare(QStringLiteral(
        "DELETE FROM t_carbon_daily"
        " WHERE stat_date = ? AND factor_version = ? AND algo_version = ?"));
    del.addBindValue(date);
    del.addBindValue(factor.version);
    del.addBindValue(QString::fromLatin1(ALGO_VERSION()));
    if (!del.exec()) {
        LOG_E(QStringLiteral("清理 %1 旧聚合行失败: %2").arg(date, del.lastError().text()));
        db.rollback();
        return ERR_INTERNAL;
    }

    int written = 0;
    for (auto it = byStation.constBegin(); it != byStation.constEnd(); ++it) {
        const StationAcc &a = it.value();

        const qint64 emission = emissionG(a.total, factor.gPerKwh);
        if (emission < 0) {                   // 纯函数已挡住越界，走到这里说明口径被改坏了
            LOG_E(QStringLiteral("%1 站 %2 排放计算越界: kwh_x100=%3 factor=%4")
                      .arg(date).arg(it.key()).arg(a.total).arg(factor.gPerKwh));
            db.rollback();
            return ERR_INTERNAL;
        }
        // 全部电量都不可分摊时如实标 UNAVAILABLE，页面据此提示口径降级
        const bool allUnalloc = (a.total > 0 && a.unalloc == a.total);

        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO t_carbon_daily"
            " (station_id, stat_date, total_kwh_x100, peak_kwh_x100, flat_kwh_x100,"
            "  valley_kwh_x100, unalloc_kwh_x100, order_cnt, emission_g,"
            "  intensity_g_per_kwh, completeness, estimated_pct, quality,"
            "  factor_version, algo_version, cutoff_time, update_time)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        ins.addBindValue(it.key());
        ins.addBindValue(date);
        ins.addBindValue(a.total);
        ins.addBindValue(a.peak);
        ins.addBindValue(a.flat);
        ins.addBindValue(a.valley);
        ins.addBindValue(a.unalloc);
        ins.addBindValue(a.cnt);
        ins.addBindValue(emission);
        ins.addBindValue(intensityGPerKwh(emission, a.total));
        ins.addBindValue(completenessPct(a.total, a.unalloc));
        // estimated_pct = UNIFORM_ESTIMATE 电量占比。MVP 中 METER_INTERVAL 永不出现，
        // 因此它恒等于 completeness；两者要到模块 02/06 接入表计数据后才会分叉。
        ins.addBindValue(completenessPct(a.total, a.total - a.estimated));
        ins.addBindValue(allUnalloc ? QStringLiteral("UNAVAILABLE")
                                    : QStringLiteral("UNIFORM_ESTIMATE"));
        ins.addBindValue(factor.version);
        ins.addBindValue(QString::fromLatin1(ALGO_VERSION()));
        ins.addBindValue(cutoffTime);
        ins.addBindValue(nowText());
        if (!ins.exec()) {
            LOG_E(QStringLiteral("写入 %1 站 %2 聚合行失败: %3")
                      .arg(date).arg(it.key()).arg(ins.lastError().text()));
            db.rollback();
            return ERR_INTERNAL;
        }
        ++written;
    }

    if (!db.commit()) {
        LOG_E(QStringLiteral("提交 %1 聚合失败: %2").arg(date, db.lastError().text()));
        db.rollback();
        return ERR_INTERNAL;
    }
    *rowsWritten = written;
    return ERR_OK;
}

// 该日是否需要重算（懒聚合判据，裁决 D4）。
//   · 今日永远重算 —— 当天的单还在陆续结算
//   · 无行但当天有单 → 从未算过
//   · 有行但 order_cnt 之和对不上实际单数 → 有迟到数据补进来了
// 判据只看**当前因子版本 + 当前算法版本**的行：换了因子就等于没算过，
// 于是自动重算并保留旧版本行（不变量 6）。
static bool dateNeedsRebuild(QSqlDatabase &db, const QString &date,
                             const QString &factorVersion, bool *ok)
{
    *ok = true;
    if (date == QDate::currentDate().toString(QLatin1String(DATE_FMT))) return true;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT COUNT(*), COALESCE(SUM(order_cnt), 0) FROM t_carbon_daily"
        " WHERE stat_date = ? AND factor_version = ? AND algo_version = ?"));
    q.addBindValue(date);
    q.addBindValue(factorVersion);
    q.addBindValue(QString::fromLatin1(ALGO_VERSION()));
    if (!q.exec() || !q.next()) {
        LOG_E(QStringLiteral("查询 %1 聚合状态失败: %2").arg(date, q.lastError().text()));
        *ok = false;
        return false;
    }
    const int rows      = q.value(0).toInt();
    const qint64 aggCnt = q.value(1).toLongLong();

    QSqlQuery c(db);
    c.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM t_order WHERE status = ? AND date(settle_time) = ?"));
    c.addBindValue(ORDER_SETTLED);
    c.addBindValue(date);
    if (!c.exec() || !c.next()) {
        LOG_E(QStringLiteral("统计 %1 订单数失败: %2").arg(date, c.lastError().text()));
        *ok = false;
        return false;
    }
    const qint64 realCnt = c.value(0).toLongLong();

    if (rows == 0) return realCnt > 0;        // 无单又无行 = 一致的空，不必反复重算
    return aggCnt != realCnt;
}

// 把 [from, to] 每一天都补齐到最新。返回实际重算的天数与行数。
static int ensureRange(QSqlDatabase &db, const QDate &from, const QDate &to,
                       const TariffPlan &plan, const QVector<Factor> &factors,
                       const QString &cutoffTime,
                       QMap<QString, QString> *factorByDate, int *daysRebuilt, int *rowsWritten,
                       QSet<QString> *rebuiltDates)
{
    for (QDate d = from; d <= to; d = d.addDays(1)) {
        const QString date = d.toString(QLatin1String(DATE_FMT));

        Factor factor;
        if (!factorForDate(factors, date, &factor)) {
            // 该日无生效因子：算不出排放，也不该编一个出来
            LOG_W(QStringLiteral("%1 无生效排放因子: %2").arg(date, errMsgExt(ERR_CARBON_NO_FACTOR)));
            return ERR_CARBON_NO_FACTOR;
        }
        factorByDate->insert(date, factor.version);

        bool ok = false;
        const bool need = dateNeedsRebuild(db, date, factor.version, &ok);
        if (!ok) return ERR_INTERNAL;
        if (!need) continue;

        int written = 0;
        const int rc = aggregateDate(db, date, plan, factor, cutoffTime, &written);
        if (rc != ERR_OK) return rc;
        *daysRebuilt += 1;
        *rowsWritten += written;
        rebuiltDates->insert(date);
    }
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  范围汇总（只读 t_carbon_daily，不触发聚合）
//
//  3740 查询、3743 生成报告、以及重算后的 STALE 判定共用这一份 ——
//  三处各写一遍求和，迟早会在某个边界上分叉，而那种分叉恰恰最难被发现：
//  数字都很像，只是对不上。
// -----------------------------------------------------------------------------
struct DayRow {
    qint64 total = 0, peak = 0, flat = 0, valley = 0, unalloc = 0, emission = 0;
    qint64 cnt = 0;
};

struct RangeTotals {
    QMap<QString, DayRow> byDate;             // QMap 天然按日期字符串升序
    DayRow        sum;
    QSet<QString> factorVersions;
    QString       minCutoff;                  // 范围内最保守的数据截止时刻
};

static int loadRangeTotals(QSqlDatabase &db, int stationId, const QDate &from, const QDate &to,
                           const QMap<QString, QString> &factorByDate, RangeTotals *out)
{
    // 只读当前算法版本的行；因子版本逐日比对 —— 范围横跨因子切换时，
    // 每天各自取自己那版，旧版本行留在表里但不参与本次汇总。
    QString sql = QStringLiteral(
        "SELECT stat_date, factor_version, station_id, total_kwh_x100, peak_kwh_x100,"
        " flat_kwh_x100, valley_kwh_x100, unalloc_kwh_x100, order_cnt, emission_g, cutoff_time"
        " FROM t_carbon_daily"
        " WHERE stat_date >= ? AND stat_date <= ? AND algo_version = ?");
    if (stationId > 0) sql += QStringLiteral(" AND station_id = ?");
    sql += QStringLiteral(" ORDER BY stat_date ASC");

    QSqlQuery q(db);
    q.prepare(sql);
    q.addBindValue(from.toString(QLatin1String(DATE_FMT)));
    q.addBindValue(to.toString(QLatin1String(DATE_FMT)));
    q.addBindValue(QString::fromLatin1(ALGO_VERSION()));
    if (stationId > 0) q.addBindValue(stationId);
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询碳日指标失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }

    while (q.next()) {
        const QString date = q.value(0).toString();
        // 该日只认它自己那一版因子的行，旧版行跳过（不变量 6：旧结果保留但不混入）
        if (factorByDate.value(date) != q.value(1).toString()) continue;

        DayRow &r = (*out).byDate[date];
        r.total    += q.value(3).toLongLong();
        r.peak     += q.value(4).toLongLong();
        r.flat     += q.value(5).toLongLong();
        r.valley   += q.value(6).toLongLong();
        r.unalloc  += q.value(7).toLongLong();
        r.cnt      += q.value(8).toLongLong();
        r.emission += q.value(9).toLongLong();
        out->factorVersions.insert(q.value(1).toString());
        const QString cut = q.value(10).toString();
        if (out->minCutoff.isEmpty() || cut < out->minCutoff) out->minCutoff = cut;
    }

    for (auto it = out->byDate.constBegin(); it != out->byDate.constEnd(); ++it) {
        const DayRow &r = it.value();
        out->sum.total   += r.total;    out->sum.peak    += r.peak;
        out->sum.flat    += r.flat;     out->sum.valley  += r.valley;
        out->sum.unalloc += r.unalloc;  out->sum.cnt     += r.cnt;
        // 总排放 = 各日排放之和，而不是拿总电量重算一遍。
        // 两者可能差几克（逐日各自四舍五入），但「表格各行相加 = 总计」
        // 才是看报表的人真正会去验的那条等式。
        out->sum.emission += r.emission;
    }
    return ERR_OK;
}

// 逐日因子版本表。范围横跨因子切换时每天各取各的（裁决 D6 保证按日选因子是精确的）。
static int buildFactorByDate(const QVector<Factor> &factors, const QDate &from, const QDate &to,
                             QMap<QString, QString> *out)
{
    for (QDate d = from; d <= to; d = d.addDays(1)) {
        const QString date = d.toString(QLatin1String(DATE_FMT));
        Factor f;
        if (!pickFactor(factors, date + QStringLiteral(" 00:00:00"), &f))
            return ERR_CARBON_NO_FACTOR;
        out->insert(date, f.version);
    }
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  重算后把受影响的 READY 报告置 STALE
//
//  08 文档第 4.2 节不变量 6：迟到或纠正数据不得静默覆盖已生成报告，
//  只能把旧版标记 STALE 并生成新版本。**报告里的数字一个字都不改** ——
//  「当时算出来是多少」本身就是报告的价值，改掉它等于把审计痕迹擦了。
//
//  只在数字**真的变了**时才标记：仅仅因为某天被重算过就把报告作废，
//  会让 STALE 角标很快失去意义（懒聚合每天都会重算今日）。
// -----------------------------------------------------------------------------
static void markStaleReports(QSqlDatabase &db, const QSet<QString> &rebuiltDates,
                             const QVector<Factor> &factors)
{
    if (rebuiltDates.isEmpty()) return;

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT report_id, station_id, date_from, date_to, total_kwh_x100, peak_kwh_x100,"
        " flat_kwh_x100, valley_kwh_x100, unalloc_kwh_x100, emission_g"
        " FROM t_carbon_report WHERE status = 'READY'"));
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询待校验报告失败: %1").arg(q.lastError().text()));
        return;
    }

    struct Candidate { int id; int stationId; QString from, to; DayRow snap; };
    QVector<Candidate> candidates;
    while (q.next()) {
        Candidate c;
        c.id            = q.value(0).toInt();
        c.stationId     = q.value(1).toInt();
        c.from          = q.value(2).toString();
        c.to            = q.value(3).toString();
        c.snap.total    = q.value(4).toLongLong();
        c.snap.peak     = q.value(5).toLongLong();
        c.snap.flat     = q.value(6).toLongLong();
        c.snap.valley   = q.value(7).toLongLong();
        c.snap.unalloc  = q.value(8).toLongLong();
        c.snap.emission = q.value(9).toLongLong();

        // 只看被重算的日子落在报告区间内的那些报告
        bool touched = false;
        for (const QString &d : rebuiltDates)
            if (d >= c.from && d <= c.to) { touched = true; break; }
        if (touched) candidates.append(c);
    }

    for (const Candidate &c : candidates) {
        const QDate from = QDate::fromString(c.from, QLatin1String(DATE_FMT));
        const QDate to   = QDate::fromString(c.to,   QLatin1String(DATE_FMT));
        if (!from.isValid() || !to.isValid()) continue;

        QMap<QString, QString> factorByDate;
        if (buildFactorByDate(factors, from, to, &factorByDate) != ERR_OK) continue;

        RangeTotals now;
        if (loadRangeTotals(db, c.stationId, from, to, factorByDate, &now) != ERR_OK) continue;

        const bool changed = now.sum.total   != c.snap.total
                          || now.sum.peak    != c.snap.peak
                          || now.sum.flat    != c.snap.flat
                          || now.sum.valley  != c.snap.valley
                          || now.sum.unalloc != c.snap.unalloc
                          || now.sum.emission!= c.snap.emission;
        if (!changed) continue;

        QSqlQuery upd(db);
        upd.prepare(QStringLiteral(
            "UPDATE t_carbon_report SET status = 'STALE' WHERE report_id = ? AND status = 'READY'"));
        upd.addBindValue(c.id);
        if (!upd.exec()) {
            LOG_E(QStringLiteral("标记报告 %1 为 STALE 失败: %2").arg(c.id).arg(upd.lastError().text()));
            continue;
        }
        LOG_W(QStringLiteral("报告 %1（%2 ~ %3）源数据已变，置为 STALE："
                             "原电量 %4 → 现 %5，原排放 %6 → 现 %7（报告数字保持不变）")
                  .arg(c.id).arg(c.from, c.to)
                  .arg(c.snap.total).arg(now.sum.total)
                  .arg(c.snap.emission).arg(now.sum.emission));
    }
}

// -----------------------------------------------------------------------------
//  入参校验：{stationId, dateFrom, dateTo}
// -----------------------------------------------------------------------------
static int parseRangeArgs(const Request &req, int *stationId, QDate *from, QDate *to)
{
    const QJsonValue sidValue = req.data.value("stationId");
    if (!sidValue.isDouble()) return ERR_PARAM;
    *stationId = sidValue.toInt(-1);
    if (*stationId < 0) return ERR_PARAM;     // 0 = 全部站点

    *from = QDate::fromString(req.data.value("dateFrom").toString(), QLatin1String(DATE_FMT));
    *to   = QDate::fromString(req.data.value("dateTo").toString(),   QLatin1String(DATE_FMT));
    if (!from->isValid() || !to->isValid()) return ERR_PARAM;
    if (*to < *from) return ERR_PARAM;
    if (from->daysTo(*to) + 1 > MAX_RANGE_DAYS) return ERR_PARAM;
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3740 CMD_EXT_CARBON_METRIC  日指标查询（内含懒聚合）
//
//  裁决 D4：不自己造定时器（00 §5.4 明令禁止），改为查询时按日核对并就地重算。
//  代价是首次查询慢一点，8292 单量级无感。
// -----------------------------------------------------------------------------
static int handleCarbonMetric(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    int stationId = 0;
    QDate from, to;
    const int pc = parseRangeArgs(req, &stationId, &from, &to);
    if (pc != ERR_OK) return pc;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("碳指标查询获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    TariffPlan plan;
    QVector<Factor> factors;
    if (!loadTariffPlan(db, &plan) || !loadFactors(db, &factors)) return ERR_INTERNAL;
    if (factors.isEmpty()) return ERR_CARBON_NO_FACTOR;

    // 先定截止时刻再取数：结果可追溯到「截到这一刻的数据」（实现规划第 5 节）
    const QString cutoffTime = nowText();
    QMap<QString, QString> factorByDate;
    QSet<QString> rebuiltDates;
    int daysRebuilt = 0, rowsWritten = 0;
    const int rc = ensureRange(db, from, to, plan, factors, cutoffTime,
                               &factorByDate, &daysRebuilt, &rowsWritten, &rebuiltDates);
    if (rc != ERR_OK) return rc;
    if (daysRebuilt > 0) {
        LOG_I(QStringLiteral("懒聚合重算 %1 天 / %2 行（%3 ~ %4）")
                  .arg(daysRebuilt).arg(rowsWritten)
                  .arg(from.toString(QLatin1String(DATE_FMT)), to.toString(QLatin1String(DATE_FMT))));
        // 重算完立刻校验既有报告：数字变了的置 STALE，报告本身的数字不动
        markStaleReports(db, rebuiltDates, factors);
    }

    RangeTotals agg;
    const int lrc = loadRangeTotals(db, stationId, from, to, factorByDate, &agg);
    if (lrc != ERR_OK) return lrc;

    QJsonArray list;
    for (auto it = agg.byDate.constBegin(); it != agg.byDate.constEnd(); ++it) {
        const DayRow &r = it.value();
        QJsonObject item;
        item["date"]             = it.key();
        item["totalKwhX100"]     = r.total;
        item["peakKwhX100"]      = r.peak;
        item["flatKwhX100"]      = r.flat;
        item["valleyKwhX100"]    = r.valley;
        item["unallocKwhX100"]   = r.unalloc;
        item["orderCnt"]         = r.cnt;
        item["emissionG"]        = r.emission;
        // 全站行的强度由「排放之和 ÷ 电量之和」得出，不是各站强度的平均 ——
        // 平均的平均会给小站过大权重，这是能源报表里最常见的一类错数
        item["intensityGPerKwh"] = intensityGPerKwh(r.emission, r.total);
        item["completeness"]     = completenessPct(r.total, r.unalloc);
        list.append(item);
    }
    const DayRow &sum = agg.sum;

    QJsonObject totals;
    totals["totalKwhX100"]     = sum.total;
    totals["peakKwhX100"]      = sum.peak;
    totals["flatKwhX100"]      = sum.flat;
    totals["valleyKwhX100"]    = sum.valley;
    totals["unallocKwhX100"]   = sum.unalloc;
    totals["orderCnt"]         = sum.cnt;
    totals["emissionG"]        = sum.emission;
    totals["intensityGPerKwh"] = intensityGPerKwh(sum.emission, sum.total);
    totals["completeness"]     = completenessPct(sum.total, sum.unalloc);

    QStringList versions = agg.factorVersions.values();
    versions.sort();                          // QSet 无序，排序后响应才可复现

    out["list"]          = list;
    out["totals"]        = totals;
    out["stationId"]     = stationId;
    out["dateFrom"]      = from.toString(QLatin1String(DATE_FMT));
    out["dateTo"]        = to.toString(QLatin1String(DATE_FMT));
    out["factorVersion"] = versions.join(QLatin1Char('/'));   // 跨因子切换时会是 "v1/v2"
    out["algoVersion"]   = QString::fromLatin1(ALGO_VERSION());
    out["cutoffTime"]    = agg.minCutoff;
    out["completeness"]  = completenessPct(sum.total, sum.unalloc);
    // 模块 05 分时电价未落地，峰平谷用固定时段。页面与导出**必须**照此标注口径
    out["tariffMode"]    = QStringLiteral("FIXED_RANGE");
    out["disclaimer"]    = disclaimerText();
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3745 CMD_EXT_CARBON_AGGREGATE  显式重算日聚合
//
//  裁决 D4 的另一半：替代未落地的 B1 定时任务，给管理端一个「立即重算」按钮。
//
//  ⚠ stationId 只做校验与回显，重算一律**按日整体进行**（含全部站点）。
//    理由同 aggregateDate()：半拉的行集对不了账。
// -----------------------------------------------------------------------------
static int handleCarbonAggregate(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    int stationId = 0;
    QDate from, to;
    const int pc = parseRangeArgs(req, &stationId, &from, &to);
    if (pc != ERR_OK) return pc;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("碳聚合重算获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    TariffPlan plan;
    QVector<Factor> factors;
    if (!loadTariffPlan(db, &plan) || !loadFactors(db, &factors)) return ERR_INTERNAL;
    if (factors.isEmpty()) return ERR_CARBON_NO_FACTOR;

    const QString cutoffTime = nowText();
    QSet<QString> rebuiltDates;
    int daysRebuilt = 0, rowsWritten = 0;

    // 显式重算 = 无条件重来，不看 dateNeedsRebuild 的判据
    for (QDate d = from; d <= to; d = d.addDays(1)) {
        const QString date = d.toString(QLatin1String(DATE_FMT));
        Factor factor;
        if (!factorForDate(factors, date, &factor)) {
            LOG_W(QStringLiteral("%1 无生效排放因子: %2").arg(date, errMsgExt(ERR_CARBON_NO_FACTOR)));
            return ERR_CARBON_NO_FACTOR;
        }
        int written = 0;
        const int rc = aggregateDate(db, date, plan, factor, cutoffTime, &written);
        if (rc != ERR_OK) return rc;
        ++daysRebuilt;
        rowsWritten += written;
        rebuiltDates.insert(date);
    }
    markStaleReports(db, rebuiltDates, factors);

    LOG_I(QStringLiteral("显式重算完成: %1 ~ %2 共 %3 天 / %4 行")
              .arg(from.toString(QLatin1String(DATE_FMT)), to.toString(QLatin1String(DATE_FMT)))
              .arg(daysRebuilt).arg(rowsWritten));

    out["days"]       = daysRebuilt;
    out["rewritten"]  = rowsWritten;
    out["stationId"]  = stationId;
    out["cutoffTime"] = cutoffTime;
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3743 CMD_EXT_REPORT_GEN  生成不可变报告版本
//
//  报告是**当时那批数字的快照**：生成后源数据再变也不覆盖它，只把它置 STALE
//  并允许生成新版本（不变量 6）。因此 t_carbon_report 存的是指标本身，
//  而不是一条「去 t_carbon_daily 现算」的引用 —— 引用会跟着源数据一起变，
//  那就不叫报告了。
//
//  幂等：req_id UNIQUE（00 第 4.6 节）。重复请求读回首次结果原样返回，不新建版本。
// -----------------------------------------------------------------------------
static int handleReportGen(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QString scope = req.data.value("scope").toString().trimmed().toUpper();
    if (scope != QLatin1String("ALL") && scope != QLatin1String("STATION")) return ERR_PARAM;

    int stationId = 0;
    QDate from, to;
    const int pc = parseRangeArgs(req, &stationId, &from, &to);
    if (pc != ERR_OK) return pc;
    // scope 与 stationId 必须自洽，否则表里会出现「全站范围却挂着某个站号」这种解释不清的行
    if (scope == QLatin1String("ALL")     && stationId != 0) return ERR_PARAM;
    if (scope == QLatin1String("STATION") && stationId <= 0) return ERR_PARAM;

    const QString reqId = req.data.value("reqId").toString().trimmed();

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("生成报告获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    // ---- 写幂等：同一 reqId 直接返回首次结果 ----
    if (!reqId.isEmpty()) {
        QSqlQuery dup(db);
        dup.prepare(QStringLiteral(
            "SELECT report_id, version, status FROM t_carbon_report WHERE req_id = ?"));
        dup.addBindValue(reqId);
        if (!dup.exec()) {
            LOG_E(QStringLiteral("查询报告幂等键失败: %1").arg(dup.lastError().text()));
            return ERR_INTERNAL;
        }
        if (dup.next()) {
            out["reportId"] = dup.value(0).toInt();
            out["version"]  = dup.value(1).toInt();
            out["status"]   = dup.value(2).toString();
            out["created"]  = false;
            LOG_I(QStringLiteral("报告请求 reqId=%1 重复提交，幂等返回 reportId=%2")
                      .arg(reqId).arg(dup.value(0).toInt()));
            return ERR_OK;
        }
    }

    TariffPlan plan;
    QVector<Factor> factors;
    if (!loadTariffPlan(db, &plan) || !loadFactors(db, &factors)) return ERR_INTERNAL;
    if (factors.isEmpty()) return ERR_CARBON_NO_FACTOR;

    // 先把范围补齐再快照。报告必须建立在最新数据上，否则刚生成就是过期的。
    const QString cutoffTime = nowText();
    QMap<QString, QString> factorByDate;
    QSet<QString> rebuiltDates;
    int daysRebuilt = 0, rowsWritten = 0;
    const int rc = ensureRange(db, from, to, plan, factors, cutoffTime,
                               &factorByDate, &daysRebuilt, &rowsWritten, &rebuiltDates);
    if (rc != ERR_OK) return rc;
    if (daysRebuilt > 0) markStaleReports(db, rebuiltDates, factors);

    RangeTotals agg;
    const int lrc = loadRangeTotals(db, stationId, from, to, factorByDate, &agg);
    if (lrc != ERR_OK) return lrc;

    QStringList versions = agg.factorVersions.values();
    versions.sort();

    const QString fromText = from.toString(QLatin1String(DATE_FMT));
    const QString toText   = to.toString(QLatin1String(DATE_FMT));

    // 同一范围重新生成 -> 版本号递增，旧版本行原样保留（不变量 6）
    QSqlQuery ver(db);
    ver.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(version), 0) + 1 FROM t_carbon_report"
        " WHERE scope = ? AND station_id = ? AND date_from = ? AND date_to = ?"));
    ver.addBindValue(scope);
    ver.addBindValue(stationId);
    ver.addBindValue(fromText);
    ver.addBindValue(toText);
    if (!ver.exec() || !ver.next()) {
        LOG_E(QStringLiteral("推算报告版本号失败: %1").arg(ver.lastError().text()));
        return ERR_INTERNAL;
    }
    const int version = ver.value(0).toInt();

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO t_carbon_report"
        " (scope, station_id, date_from, date_to, version, status,"
        "  total_kwh_x100, peak_kwh_x100, flat_kwh_x100, valley_kwh_x100, unalloc_kwh_x100,"
        "  emission_g, intensity_g_per_kwh, completeness, factor_version, algo_version,"
        "  tariff_mode, cutoff_time, run_time, output_path, req_id)"
        " VALUES (?,?,?,?,?,'READY',?,?,?,?,?,?,?,?,?,?,?,?,?,'',?)"));
    ins.addBindValue(scope);
    ins.addBindValue(stationId);
    ins.addBindValue(fromText);
    ins.addBindValue(toText);
    ins.addBindValue(version);
    ins.addBindValue(agg.sum.total);
    ins.addBindValue(agg.sum.peak);
    ins.addBindValue(agg.sum.flat);
    ins.addBindValue(agg.sum.valley);
    ins.addBindValue(agg.sum.unalloc);
    ins.addBindValue(agg.sum.emission);
    ins.addBindValue(intensityGPerKwh(agg.sum.emission, agg.sum.total));
    ins.addBindValue(completenessPct(agg.sum.total, agg.sum.unalloc));
    ins.addBindValue(versions.join(QLatin1Char('/')));
    ins.addBindValue(QString::fromLatin1(ALGO_VERSION()));
    ins.addBindValue(QStringLiteral("FIXED_RANGE"));   // 模块 05 未落地，固定时段口径
    ins.addBindValue(agg.minCutoff);
    ins.addBindValue(nowText());
    // req_id 为空时存 NULL：UNIQUE 列里多行空串会互相冲突，NULL 则彼此不冲突
    ins.addBindValue(reqId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(reqId));
    if (!ins.exec()) {
        // 并发下同一 reqId 可能刚被别的线程插入 —— 读回来返回，不当成错误
        if (!reqId.isEmpty()) {
            QSqlQuery again(db);
            again.prepare(QStringLiteral(
                "SELECT report_id, version, status FROM t_carbon_report WHERE req_id = ?"));
            again.addBindValue(reqId);
            if (again.exec() && again.next()) {
                out["reportId"] = again.value(0).toInt();
                out["version"]  = again.value(1).toInt();
                out["status"]   = again.value(2).toString();
                out["created"]  = false;
                return ERR_OK;
            }
        }
        LOG_E(QStringLiteral("写入报告失败: %1").arg(ins.lastError().text()));
        return ERR_INTERNAL;
    }

    const int reportId = ins.lastInsertId().toInt();
    out["reportId"] = reportId;
    out["version"]  = version;
    out["status"]   = QStringLiteral("READY");
    out["created"]  = true;
    LOG_I(QStringLiteral("生成报告 %1 v%2: %3 %4 ~ %5，电量 %6，排放 %7 g，因子 %8")
              .arg(reportId).arg(version).arg(scope, fromText, toText)
              .arg(agg.sum.total).arg(agg.sum.emission).arg(versions.join(QLatin1Char('/'))));
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3746 CMD_EXT_REPORT_LIST  报告列表（含 STALE 状态）
// -----------------------------------------------------------------------------
static int handleReportList(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("报告列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT report_id, scope, station_id, date_from, date_to, version, status,"
        " total_kwh_x100, peak_kwh_x100, flat_kwh_x100, valley_kwh_x100, unalloc_kwh_x100,"
        " emission_g, intensity_g_per_kwh, completeness, factor_version, algo_version,"
        " tariff_mode, cutoff_time, run_time, output_path"
        " FROM t_carbon_report ORDER BY run_time DESC, report_id DESC LIMIT 200"));
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询报告列表失败: %1").arg(q.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (q.next()) {
        QJsonObject item;
        item["reportId"]         = q.value(0).toInt();
        item["scope"]            = q.value(1).toString();
        item["stationId"]        = q.value(2).toInt();
        item["dateFrom"]         = q.value(3).toString();
        item["dateTo"]           = q.value(4).toString();
        item["version"]          = q.value(5).toInt();
        item["status"]           = q.value(6).toString();
        item["totalKwhX100"]     = q.value(7).toLongLong();
        item["peakKwhX100"]      = q.value(8).toLongLong();
        item["flatKwhX100"]      = q.value(9).toLongLong();
        item["valleyKwhX100"]    = q.value(10).toLongLong();
        item["unallocKwhX100"]   = q.value(11).toLongLong();
        item["emissionG"]        = q.value(12).toLongLong();
        item["intensityGPerKwh"] = q.value(13).toLongLong();
        item["completeness"]     = q.value(14).toInt();
        item["factorVersion"]    = q.value(15).toString();
        item["algoVersion"]      = q.value(16).toString();
        item["tariffMode"]       = q.value(17).toString();
        item["cutoffTime"]       = q.value(18).toString();
        item["runTime"]          = q.value(19).toString();
        item["outputPath"]       = q.value(20).toString();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3744 CMD_EXT_REPORT_EXPORT  导出 CSV / 可打印 HTML
//
//  文件落盘，**路径走 Socket，文件本身不走 Socket**（docs/protocol.md 第 2 节）。
//  因此假定管理端与服务端同机（演示场景）—— 这一条必须写在页面上，
//  否则异机部署时用户会拿着一个本地不存在的路径发愣。
//
//  导出的数字一律取自报告行的快照列，不去 t_carbon_daily 现算。
//  STALE 报告导出的就该是「当时那个数」，这正是它存在的理由。
//
//  STALE 默认拒绝导出并返回 6703，客户端确认后带 allowStale 重发 ——
//  既不让人误把过期数字当成最新，也保住「旧数字仍可查」。
// -----------------------------------------------------------------------------

// 整数转两位小数文本，全程不碰浮点（与管理端 twoDecimals 同一口径）
static QString twoDecimals(qint64 hundredths)
{
    const qint64 sign = hundredths < 0 ? -1 : 1;
    const qint64 v = hundredths * sign;
    return QStringLiteral("%1%2.%3").arg(sign < 0 ? QStringLiteral("-") : QString())
                                    .arg(v / 100)
                                    .arg(v % 100, 2, 10, QLatin1Char('0'));
}

struct ReportRow {
    int     reportId = 0, stationId = 0, version = 0, completeness = -1;
    QString scope, dateFrom, dateTo, status, factorVersion, algoVersion, tariffMode;
    QString cutoffTime, runTime;
    qint64  total = 0, peak = 0, flat = 0, valley = 0, unalloc = 0, emission = 0, intensity = -1;
};

static QString reportTitle(const ReportRow &r)
{
    return QStringLiteral("碳排放报告 · %1 · %2 ~ %3 · v%4")
        .arg(r.scope == QLatin1String("ALL") ? QStringLiteral("全部电站")
                                             : QStringLiteral("电站 %1").arg(r.stationId))
        .arg(r.dateFrom, r.dateTo).arg(r.version);
}

static QString staleBanner(const ReportRow &r)
{
    if (r.status != QLatin1String("STALE")) return QString();
    return QStringLiteral("本报告已过期：生成之后源数据发生了变化。"
                          "下方数字是 %1 生成当时的快照，未被覆盖；"
                          "如需最新数字请重新生成报告。").arg(r.runTime);
}

static QString intensityText(qint64 v)
{
    return v == NOT_APPLICABLE_VALUE ? QStringLiteral("不适用") : QString::number(v);
}

static QString completenessText(int v)
{
    return v == NOT_APPLICABLE_VALUE ? QStringLiteral("不适用") : QString::number(v);
}

static QString buildReportCsv(const ReportRow &r, const QMap<QString, DayRow> &detail)
{
    QStringList lines;
    lines << QStringLiteral("# %1").arg(reportTitle(r));
    lines << QStringLiteral("# 免责声明: %1").arg(disclaimerText());
    lines << QStringLiteral("# 口径: %1（峰 10:00-15:00、18:00-21:00；谷 23:00-07:00；"
                            "平 = 24h 扣除峰谷；区间左闭右开，跨午夜按真实钟点切分）")
                 .arg(r.tariffMode == QLatin1String("FIXED_RANGE") ? QStringLiteral("固定时段")
                                                                   : r.tariffMode);
    lines << QStringLiteral("# 因子版本: %1  算法版本: %2  数据截止: %3  生成时间: %4  状态: %5")
                 .arg(r.factorVersion, r.algoVersion, r.cutoffTime, r.runTime, r.status);
    const QString banner = staleBanner(r);
    if (!banner.isEmpty()) lines << QStringLiteral("# [警告] %1").arg(banner);
    lines << QString();

    lines << QStringLiteral("指标,数值,单位");
    lines << QStringLiteral("总充电量,%1,度").arg(twoDecimals(r.total));
    lines << QStringLiteral("峰段电量,%1,度").arg(twoDecimals(r.peak));
    lines << QStringLiteral("平段电量,%1,度").arg(twoDecimals(r.flat));
    lines << QStringLiteral("谷段电量,%1,度").arg(twoDecimals(r.valley));
    lines << QStringLiteral("未分摊电量,%1,度").arg(twoDecimals(r.unalloc));
    lines << QStringLiteral("估算碳排放,%1,kg").arg(twoDecimals((r.emission + 5) / 10));
    lines << QStringLiteral("排放强度,%1,g/度").arg(intensityText(r.intensity));
    lines << QStringLiteral("数据完整度,%1,百分比").arg(completenessText(r.completeness));
    lines << QString();

    if (r.status == QLatin1String("STALE")) {
        // 过期报告不附每日明细：明细只能来自 t_carbon_daily 的**当前**数据，
        // 与上方的历史快照不是同一批数字，并排放在一张表里只会误导人。
        lines << QStringLiteral("# 每日明细已省略：报告已过期，当前明细与上方快照并非同一批数据");
    } else {
        lines << QStringLiteral("日期,总电量(度),峰(度),平(度),谷(度),未分摊(度),"
                                "订单数,估算排放(kg),完整度");
        for (auto it = detail.constBegin(); it != detail.constEnd(); ++it) {
            const DayRow &d = it.value();
            lines << QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9")
                         .arg(it.key())
                         .arg(twoDecimals(d.total), twoDecimals(d.peak), twoDecimals(d.flat))
                         .arg(twoDecimals(d.valley), twoDecimals(d.unalloc))
                         .arg(d.cnt)
                         .arg(twoDecimals((d.emission + 5) / 10))
                         .arg(completenessText(completenessPct(d.total, d.unalloc)));
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

static QString htmlEscape(const QString &text)
{
    QString v = text;
    v.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    v.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    v.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return v;
}

static QString buildReportHtml(const ReportRow &r, const QMap<QString, DayRow> &detail)
{
    QStringList rows;
    if (r.status != QLatin1String("STALE")) {
        for (auto it = detail.constBegin(); it != detail.constEnd(); ++it) {
            const DayRow &d = it.value();
            rows << QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td>"
                                   "<td>%5</td><td>%6</td><td>%7</td><td>%8</td><td>%9</td></tr>")
                        .arg(it.key())
                        .arg(twoDecimals(d.total), twoDecimals(d.peak), twoDecimals(d.flat))
                        .arg(twoDecimals(d.valley), twoDecimals(d.unalloc))
                        .arg(d.cnt)
                        .arg(twoDecimals((d.emission + 5) / 10))
                        .arg(completenessText(completenessPct(d.total, d.unalloc)));
        }
    }
    const QString banner = staleBanner(r);
    const QString staleBlock = banner.isEmpty()
        ? QString()
        : QStringLiteral("<div class=\"warn stale\">[警告] %1</div>\n").arg(htmlEscape(banner));
    const QString detailBlock = rows.isEmpty()
        ? QStringLiteral("<p class=\"sub\">每日明细已省略：报告已过期，"
                         "当前明细与上方快照并非同一批数据。</p>\n")
        : QStringLiteral("<h2 style=\"font-size:15px;margin-top:22px\">每日明细</h2>\n"
              "<table><tr><th>日期</th><th>总电量(度)</th><th>峰(度)</th><th>平(度)</th>"
              "<th>谷(度)</th><th>未分摊(度)</th><th>订单数</th><th>估算排放(kg)</th>"
              "<th>完整度</th></tr>\n%1</table>\n").arg(rows.join(QLatin1Char('\n')));

    return QStringLiteral(
        "<!doctype html>\n<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n"
        "<title>%1</title>\n<style>\n"
        "body{font-family:\"Noto Sans CJK SC\",sans-serif;margin:32px;color:#222;line-height:1.6}\n"
        "h1{font-size:20px;margin:0 0 4px} .sub{color:#666;font-size:13px;margin-bottom:14px}\n"
        ".warn{background:#fff7e6;border:1px solid #ffd591;color:#874d00;"
        "padding:10px 14px;border-radius:4px;margin:10px 0;font-size:13px}\n"
        ".stale{background:#fff1f0;border-color:#ffa39e;color:#a8071a}\n"
        "table{border-collapse:collapse;margin-top:12px;font-size:13px}\n"
        "th,td{border:1px solid #ddd;padding:5px 10px;text-align:right}\n"
        "th{background:#fafafa} td:first-child,th:first-child{text-align:left}\n"
        "@media print{body{margin:12mm}}\n"
        "</style></head><body>\n"
        "<h1>%1</h1>\n<div class=\"sub\">因子版本 %2　算法版本 %3　数据截止 %4　"
        "生成时间 %5　状态 %6</div>\n"
        "%7"
        "<div class=\"warn\">[免责声明] %8</div>\n"
        "<div class=\"warn\">口径：%9（峰 10:00-15:00、18:00-21:00；谷 23:00-07:00；"
        "平 = 24h 扣除峰谷）。区间左闭右开，跨午夜订单按真实钟点切分，日归属按结算时刻。</div>\n"
        "<table><tr><th>指标</th><th>数值</th></tr>\n"
        "<tr><td>总充电量</td><td>%10 度</td></tr>\n"
        "<tr><td>峰段电量</td><td>%11 度</td></tr>\n"
        "<tr><td>平段电量</td><td>%12 度</td></tr>\n"
        "<tr><td>谷段电量</td><td>%13 度</td></tr>\n"
        "<tr><td>未分摊电量</td><td>%14 度</td></tr>\n"
        "<tr><td>估算碳排放</td><td>%15 kg</td></tr>\n"
        "<tr><td>排放强度</td><td>%16 g/度</td></tr>\n"
        "<tr><td>数据完整度</td><td>%17</td></tr>\n</table>\n"
        "%18"
        "</body></html>\n")
        .arg(htmlEscape(reportTitle(r)))
        .arg(htmlEscape(r.factorVersion), htmlEscape(r.algoVersion),
             htmlEscape(r.cutoffTime), htmlEscape(r.runTime), htmlEscape(r.status))
        .arg(staleBlock)
        .arg(htmlEscape(disclaimerText()))
        .arg(r.tariffMode == QLatin1String("FIXED_RANGE") ? QStringLiteral("固定时段")
                                                          : htmlEscape(r.tariffMode))
        .arg(twoDecimals(r.total), twoDecimals(r.peak), twoDecimals(r.flat))
        .arg(twoDecimals(r.valley), twoDecimals(r.unalloc), twoDecimals((r.emission + 5) / 10))
        .arg(intensityText(r.intensity))
        .arg(completenessText(r.completeness))
        .arg(detailBlock);
}

static int handleReportExport(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QJsonValue idValue = req.data.value("reportId");
    if (!idValue.isDouble()) return ERR_PARAM;
    const int reportId = idValue.toInt(0);
    if (reportId <= 0) return ERR_PARAM;

    const QString format = req.data.value("format").toString().trimmed().toLower();
    if (format != QLatin1String("csv") && format != QLatin1String("html")) return ERR_PARAM;
    const bool allowStale = req.data.value("allowStale").toBool(false);

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("导出报告获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT report_id, scope, station_id, date_from, date_to, version, status,"
        " total_kwh_x100, peak_kwh_x100, flat_kwh_x100, valley_kwh_x100, unalloc_kwh_x100,"
        " emission_g, intensity_g_per_kwh, completeness, factor_version, algo_version,"
        " tariff_mode, cutoff_time, run_time FROM t_carbon_report WHERE report_id = ?"));
    q.addBindValue(reportId);
    if (!q.exec()) {
        LOG_E(QStringLiteral("查询报告 %1 失败: %2").arg(reportId).arg(q.lastError().text()));
        return ERR_INTERNAL;
    }
    if (!q.next()) {
        LOG_W(QStringLiteral("导出报告 %1: %2").arg(reportId)
                  .arg(errMsgExt(ERR_CARBON_REPORT_NOT_FOUND)));
        return ERR_CARBON_REPORT_NOT_FOUND;
    }

    ReportRow r;
    r.reportId      = q.value(0).toInt();
    r.scope         = q.value(1).toString();
    r.stationId     = q.value(2).toInt();
    r.dateFrom      = q.value(3).toString();
    r.dateTo        = q.value(4).toString();
    r.version       = q.value(5).toInt();
    r.status        = q.value(6).toString();
    r.total         = q.value(7).toLongLong();
    r.peak          = q.value(8).toLongLong();
    r.flat          = q.value(9).toLongLong();
    r.valley        = q.value(10).toLongLong();
    r.unalloc       = q.value(11).toLongLong();
    r.emission      = q.value(12).toLongLong();
    r.intensity     = q.value(13).toLongLong();
    r.completeness  = q.value(14).toInt();
    r.factorVersion = q.value(15).toString();
    r.algoVersion   = q.value(16).toString();
    r.tariffMode    = q.value(17).toString();
    r.cutoffTime    = q.value(18).toString();
    r.runTime       = q.value(19).toString();

    // 过期报告默认不导出：先让人看见「这是旧数字」，确认后再带 allowStale 重发
    if (r.status == QLatin1String("STALE") && !allowStale) {
        LOG_W(QStringLiteral("报告 %1 已过期，未确认不予导出: %2")
                  .arg(reportId).arg(errMsgExt(ERR_CARBON_REPORT_STALE)));
        return ERR_CARBON_REPORT_STALE;
    }
    if (r.status == QLatin1String("GENERATING") || r.status == QLatin1String("FAILED")) {
        LOG_W(QStringLiteral("报告 %1 状态为 %2，不可导出").arg(reportId).arg(r.status));
        return ERR_PARAM;
    }

    // READY 报告才附每日明细：此时 t_carbon_daily 与快照同源，数字对得上
    QMap<QString, DayRow> detail;
    if (r.status == QLatin1String("READY")) {
        QVector<Factor> factors;
        if (!loadFactors(db, &factors)) return ERR_INTERNAL;
        const QDate from = QDate::fromString(r.dateFrom, QLatin1String(DATE_FMT));
        const QDate to   = QDate::fromString(r.dateTo,   QLatin1String(DATE_FMT));
        QMap<QString, QString> factorByDate;
        if (from.isValid() && to.isValid()
            && buildFactorByDate(factors, from, to, &factorByDate) == ERR_OK) {
            RangeTotals agg;
            if (loadRangeTotals(db, r.stationId, from, to, factorByDate, &agg) == ERR_OK)
                detail = agg.byDate;
        }
    }

    const QString content = (format == QLatin1String("csv")) ? buildReportCsv(r, detail)
                                                             : buildReportHtml(r, detail);

    const QString dirPath = QStringLiteral("export/carbon");
    QDir().mkpath(dirPath);
    const QString relPath = QStringLiteral("%1/report_%2_v%3.%4")
                                .arg(dirPath).arg(r.reportId).arg(r.version).arg(format);

    QFile file(relPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOG_E(QStringLiteral("打开导出文件 %1 失败: %2").arg(relPath, file.errorString()));
        return ERR_INTERNAL;
    }
    // CSV 加 UTF-8 BOM：不加的话 Excel 会按本地编码打开，中文表头全是乱码
    if (format == QLatin1String("csv")) file.write("\xEF\xBB\xBF");
    file.write(content.toUtf8());
    file.close();

    QSqlQuery upd(db);
    upd.prepare(QStringLiteral("UPDATE t_carbon_report SET output_path = ? WHERE report_id = ?"));
    upd.addBindValue(relPath);
    upd.addBindValue(r.reportId);
    if (!upd.exec())
        LOG_W(QStringLiteral("回写报告 %1 导出路径失败: %2").arg(r.reportId).arg(upd.lastError().text()));

    out["path"]     = relPath;
    out["absPath"]  = QFileInfo(relPath).absoluteFilePath();
    out["format"]   = format;
    out["stale"]    = r.status == QLatin1String("STALE");
    out["reportId"] = r.reportId;
    LOG_I(QStringLiteral("导出报告 %1 v%2 -> %3（状态 %4）")
              .arg(r.reportId).arg(r.version).arg(relPath, r.status));
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3742 CMD_EXT_FACTOR_SET  新增排放因子版本
//
//  ⚠ 因子版本一经写入即**不可变**：同一 (region, version) 再次提交，
//    内容相同 → 幂等返回原 factorId；内容不同 → ERR_PARAM，要求发新 version。
//    理由是不变量 6 —— t_carbon_daily 与 t_carbon_report 都靠 factor_version
//    追溯当时用的是哪个数。允许就地改内容，等于让所有历史结果的出处凭空变样。
//    因此本命令只「新增版本」，不提供修改与启用/停用。
//
//  重叠校验只在**启用中**的因子之间做（停用的不参与 pickFactor，重叠也不产生歧义）。
//  由于本命令不提供重新启用，不存在「先停用再启用造成重叠」的隐患。
//
//  写入后不必手动重算：新因子版本在 t_carbon_daily 里没有对应行，
//  下一次 3740 的懒聚合判据自然认定「没算过」并重算，旧版本行原样保留。
// -----------------------------------------------------------------------------
static int handleFactorSet(const Request &req, QJsonObject &out)
{
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    const QString region  = req.data.value("region").toString().trimmed();
    const QString version = req.data.value("version").toString().trimmed();
    const QString source  = req.data.value("source").toString().trimmed();
    if (region.isEmpty() || version.isEmpty()) return ERR_PARAM;
    // 08 文档第 9 节把「排放因子来源不可靠」列为风险：没有来源说明的因子不许入库
    if (source.isEmpty()) return ERR_PARAM;

    const QJsonValue gValue = req.data.value("factorGPerKwh");
    if (!gValue.isDouble()) return ERR_PARAM;
    const qint64 gPerKwh = gValue.toInteger(0);
    if (gPerKwh <= 0) return ERR_PARAM;              // 与建表 CHECK 一致

    const QString effectFrom = req.data.value("effectFrom").toString().trimmed();
    const QString effectTo   = req.data.value("effectTo").toString().trimmed();
    // 裁决 D6：生效边界必须对齐自然日 00:00:00，否则一天会横跨两个因子版本
    if (!isDayAlignedBoundary(effectFrom)) return ERR_PARAM;
    if (!effectTo.isEmpty()) {
        if (!isDayAlignedBoundary(effectTo)) return ERR_PARAM;
        if (effectTo <= effectFrom) return ERR_PARAM;   // 与建表 CHECK 一致
    }

    QSqlDatabase db = threadDb();
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("新增排放因子获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    // ---- 幂等：(region, version) 是写幂等键（建表 UNIQUE），不另设 req_id 列 ----
    QSqlQuery dup(db);
    dup.prepare(QStringLiteral(
        "SELECT factor_id, effect_from, effect_to, factor_g_per_kwh, source"
        " FROM t_carbon_factor WHERE region = ? AND version = ?"));
    dup.addBindValue(region);
    dup.addBindValue(version);
    if (!dup.exec()) {
        LOG_E(QStringLiteral("查询因子重名失败: %1").arg(dup.lastError().text()));
        return ERR_INTERNAL;
    }
    if (dup.next()) {
        const QString oldTo = dup.value(2).isNull() ? QString() : dup.value(2).toString();
        const bool same = dup.value(1).toString() == effectFrom
                       && oldTo == effectTo
                       && dup.value(3).toLongLong() == gPerKwh
                       && dup.value(4).toString() == source;
        if (!same) {
            LOG_W(QStringLiteral("因子 %1/%2 已存在且内容不同，拒绝就地修改（请发新 version）")
                      .arg(region, version));
            return ERR_PARAM;
        }
        out["factorId"] = dup.value(0).toInt();
        out["created"]  = false;                     // 重复提交，原样返回首次结果
        LOG_I(QStringLiteral("因子 %1/%2 重复提交，幂等返回 factorId=%3")
                  .arg(region, version).arg(dup.value(0).toInt()));
        return ERR_OK;
    }

    // ---- 生效区间重叠校验（SQLite 表约束表达不了区间重叠，只能在这里做）----
    QVector<Factor> existing;
    if (!loadFactors(db, &existing)) return ERR_INTERNAL;

    Factor incoming;
    incoming.region     = region;
    incoming.version    = version;
    incoming.effectFrom = effectFrom;
    incoming.effectTo   = effectTo;
    incoming.gPerKwh    = gPerKwh;
    incoming.source     = source;
    if (factorOverlaps(existing, incoming)) {
        LOG_W(QStringLiteral("因子 %1/%2 [%3, %4) %5")
                  .arg(region, version, effectFrom,
                       effectTo.isEmpty() ? QStringLiteral("∞") : effectTo,
                       errMsgExt(ERR_CARBON_FACTOR_OVERLAP)));
        return ERR_CARBON_FACTOR_OVERLAP;
    }

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO t_carbon_factor"
        " (region, version, effect_from, effect_to, factor_g_per_kwh, source, enabled, create_time)"
        " VALUES (?,?,?,?,?,?,1,?)"));
    ins.addBindValue(region);
    ins.addBindValue(version);
    ins.addBindValue(effectFrom);
    // 空串存 NULL：建表用 effect_to IS NULL 表示右开无穷，存空串会让区间判断全线失灵
    ins.addBindValue(effectTo.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(effectTo));
    ins.addBindValue(gPerKwh);
    ins.addBindValue(source);
    ins.addBindValue(nowText());
    if (!ins.exec()) {
        LOG_E(QStringLiteral("写入排放因子失败: %1").arg(ins.lastError().text()));
        return ERR_INTERNAL;
    }

    const int factorId = ins.lastInsertId().toInt();
    out["factorId"] = factorId;
    out["created"]  = true;
    LOG_I(QStringLiteral("新增排放因子 %1/%2 = %3 g/度，生效 [%4, %5)，factorId=%6。"
                         "受影响日期将在下次 3740 查询时自动重算，旧版本行保留")
              .arg(region, version).arg(gPerKwh)
              .arg(effectFrom, effectTo.isEmpty() ? QStringLiteral("∞") : effectTo)
              .arg(factorId));
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  3741 CMD_EXT_FACTOR_LIST  排放因子版本列表
//
//  纯只读，无计算。S1 用它把整条链路先打通：扩展头文件、鉴权门、功能开关、
//  ext 建表脚本是否真的执行过 —— 这些出问题都会在这里先暴露，
//  而不是等到 S3 的聚合逻辑里混着算错的数字一起排查。
// -----------------------------------------------------------------------------
static int handleFactorList(const Request &req, QJsonObject &out)
{
    // 与 2301 一致：管理端命令一律校验角色（server/biz/statistics_service.cpp）
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();          // 硬性规则 2：禁止跨线程共享连接
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("排放因子列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);                   // 硬性规则：SQL 一律 prepare + bindValue
    query.prepare(QStringLiteral(
        "SELECT factor_id, region, version, effect_from, effect_to,"
        " factor_g_per_kwh, source, enabled"
        " FROM t_carbon_factor ORDER BY region, effect_from, version"));
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询排放因子列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["factorId"]      = query.value(0).toInt();
        item["region"]        = query.value(1).toString();
        item["version"]       = query.value(2).toString();
        item["effectFrom"]    = query.value(3).toString();
        // effect_to 为 NULL 表示右开无穷（08 文档第 4.2 节不变量 5：区间左闭右开）
        item["effectTo"]      = query.value(4).isNull() ? QString() : query.value(4).toString();
        item["factorGPerKwh"] = query.value(5).toInt();
        item["source"]        = query.value(6).toString();
        item["enabled"]       = query.value(7).toInt();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  注册入口。server/main.cpp 的 registerAllServices() 只加一行调用它，
//  且外面包着 feat_08_carbon 功能开关判断（00 第 4.7 节）。
// -----------------------------------------------------------------------------
void registerExt08CarbonService()
{
    Dispatcher::instance().registerHandler(CMD_EXT_CARBON_METRIC,    handleCarbonMetric);
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_LIST,      handleFactorList);
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_SET,       handleFactorSet);
    Dispatcher::instance().registerHandler(CMD_EXT_CARBON_AGGREGATE, handleCarbonAggregate);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_GEN,       handleReportGen);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_EXPORT,    handleReportExport);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_LIST,      handleReportList);

    LOG_I(QStringLiteral("扩展模块 08 碳减排与能源报告已注册: 3740 日指标、3741 因子列表、"
                         "3742 新增因子、3743 生成报告、3744 导出、3745 显式重算、3746 报告列表"));
}

} // namespace ecp
