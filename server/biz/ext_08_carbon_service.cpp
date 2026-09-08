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
//            → **S4 因子管理（本文件当前状态）** → S5 报告导出与大屏
// -----------------------------------------------------------------------------
#include <QDate>
#include <QDateTime>
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
                       QMap<QString, QString> *factorByDate, int *daysRebuilt, int *rowsWritten)
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
    }
    return ERR_OK;
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
    int daysRebuilt = 0, rowsWritten = 0;
    const int rc = ensureRange(db, from, to, plan, factors, cutoffTime,
                               &factorByDate, &daysRebuilt, &rowsWritten);
    if (rc != ERR_OK) return rc;
    if (daysRebuilt > 0)
        LOG_I(QStringLiteral("懒聚合重算 %1 天 / %2 行（%3 ~ %4）")
                  .arg(daysRebuilt).arg(rowsWritten)
                  .arg(from.toString(QLatin1String(DATE_FMT)), to.toString(QLatin1String(DATE_FMT))));

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

    struct DayRow {
        qint64 total = 0, peak = 0, flat = 0, valley = 0, unalloc = 0, emission = 0;
        qint64 cnt = 0;
    };
    QMap<QString, DayRow> byDate;             // QMap 天然按日期字符串升序
    QSet<QString> usedFactorVersions;
    QString minCutoff;

    while (q.next()) {
        const QString date = q.value(0).toString();
        // 该日只认它自己那一版因子的行，旧版行跳过（不变量 6：旧结果保留但不混入）
        if (factorByDate.value(date) != q.value(1).toString()) continue;

        DayRow &r = byDate[date];
        r.total    += q.value(3).toLongLong();
        r.peak     += q.value(4).toLongLong();
        r.flat     += q.value(5).toLongLong();
        r.valley   += q.value(6).toLongLong();
        r.unalloc  += q.value(7).toLongLong();
        r.cnt      += q.value(8).toLongLong();
        r.emission += q.value(9).toLongLong();
        usedFactorVersions.insert(q.value(1).toString());
        const QString cut = q.value(10).toString();
        if (minCutoff.isEmpty() || cut < minCutoff) minCutoff = cut;   // 取最保守的截止时刻
    }

    QJsonArray list;
    DayRow sum;
    for (auto it = byDate.constBegin(); it != byDate.constEnd(); ++it) {
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

        sum.total += r.total;  sum.peak    += r.peak;     sum.flat  += r.flat;
        sum.valley+= r.valley; sum.unalloc += r.unalloc;  sum.cnt   += r.cnt;
        // 总排放 = 各日排放之和，而不是拿总电量重算一遍。
        // 两者可能差几克（逐日各自四舍五入），但「表格各行相加 = 总计」
        // 才是看报表的人真正会去验的那条等式。
        sum.emission += r.emission;
    }

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

    QStringList versions = usedFactorVersions.values();
    versions.sort();                          // QSet 无序，排序后响应才可复现

    out["list"]          = list;
    out["totals"]        = totals;
    out["stationId"]     = stationId;
    out["dateFrom"]      = from.toString(QLatin1String(DATE_FMT));
    out["dateTo"]        = to.toString(QLatin1String(DATE_FMT));
    out["factorVersion"] = versions.join(QLatin1Char('/'));   // 跨因子切换时会是 "v1/v2"
    out["algoVersion"]   = QString::fromLatin1(ALGO_VERSION());
    out["cutoffTime"]    = minCutoff;
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
    }

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

    LOG_I(QStringLiteral("扩展模块 08 碳减排与能源报告已注册: 3740 日指标查询、"
                         "3741 因子列表、3742 新增因子、3745 显式重算"
                         "（3743/3744/3746 报告与导出待 S5 落地）"));
}

} // namespace ecp
