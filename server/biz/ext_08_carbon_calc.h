#pragma once
// -----------------------------------------------------------------------------
//  server/biz/ext_08_carbon_calc.h  —  扩展模块 08 碳排放计算核心
//  归属 L5（模块 08 认领人）
//
//  **纯函数，零数据库依赖、零全局状态。** 所有手算夹具打在这一层
//  （server/biz/ext_08_carbon_calc_test.cpp，用 scripts/test-carbon-calc.sh 跑）。
//  把「算得对不对」与「取数对不对」分开，是这个模块唯一能被手工复核的地方。
//
//  口径已冻结，见 docs/expand/08-实现规划.md 第 2 节：
//    · 电量 kwh_x100（度 × 100 整数），排放 emission_g（整数克），因子 g/度 整数
//    · 四舍五入一律 (a*b + 除数/2) / 除数 的整数写法，**全程不出现 double/float**
//    · 完整度与强度：分母为 0 时返回 -1「不适用」，绝不伪装成 100
//    · 分时守恒：peak + flat + valley + unalloc ≡ total，逐单成立
//
//  ⚠ 本项目单时区、本地时间，且中国不实行夏令时，故区间运算不做时区换算
//    （CLAUDE.md 第 2 节 + 08 实现规划裁决 D3）。
// -----------------------------------------------------------------------------
#include <QDateTime>
#include <QString>
#include <QVector>

namespace ecp {
namespace carbon {

// 算法版本。**改动任何算式或口径都必须同时改它** —— 它写进 t_carbon_daily 的
// UNIQUE 键，旧结果因此得以保留而不是被静默覆盖（08 文档第 4.2 节不变量 4、6）。
inline const char *ALGO_VERSION() { return "carbon-v1"; }

// 分母为 0 时的「不适用」标记，见 00 第 4.5 节：不伪装成 100
constexpr int  NOT_APPLICABLE = -1;
// 单笔电量上限，防止脏数据把中间乘法推到 qint64 边界。1e11 = 10 亿度，远超任何真实订单
constexpr qint64 KWH_X100_MAX = 100000000000LL;
// 单次会话时长上限 7 天。超过即视为脏数据，不强行分摊（08 文档第 3.2 节第 3 档）
constexpr qint64 SESSION_SEC_MAX = 7 * 24 * 3600;

// 数据质量。取值必须与 docs/db-schema-ext-08.sql 的 CHECK 一致。
// ⚠ METER_INTERVAL 在 MVP 中永不出现：t_order 没有表计区间数据。
//   待模块 02 / 06 接入后才可能取到，枚举先予保留。
enum Quality { QUALITY_METER_INTERVAL, QUALITY_UNIFORM_ESTIMATE, QUALITY_UNAVAILABLE };
QString qualityName(Quality q);

// 一天之内的时段片，[fromSec, toSec) 自当日 00:00:00 起的秒数，0 <= from < to <= 86400。
// 跨午夜的配置（如 23:00-07:00）在解析时就被拆成两片，运算层不必再处理回绕。
struct TimeSlice {
    int fromSec = 0;
    int toSec   = 0;
};

// 峰谷时段表。平段 = 24h 扣掉峰段与谷段，不单独配置 —— 三段都配会出现「配不满
// 24 小时」的无解状态，见 docs/db-schema-ext-08.sql 种子数据注释。
struct TariffPlan {
    QVector<TimeSlice> peak;
    QVector<TimeSlice> valley;
    bool    valid = false;
    QString error;          // valid=false 时说明原因，供服务端记日志
};

// 解析 t_sys_config 的 carbon_peak_ranges / carbon_valley_ranges，
// 格式 "HH:MM-HH:MM,HH:MM-HH:MM"。峰谷自身重叠或互相重叠一律判为非法。
TariffPlan parseTariffPlan(const QString &peakRanges, const QString &valleyRanges);

// 单笔订单的分时分摊结果，单位 kwh_x100
struct SplitResult {
    qint64  peak    = 0;
    qint64  flat    = 0;
    qint64  valley  = 0;
    qint64  unalloc = 0;
    Quality quality = QUALITY_UNAVAILABLE;
    qint64  total() const { return peak + flat + valley + unalloc; }
};

// 按 [start, end) 与各时段的**重叠秒数**分摊电量，最大余数法保证四项之和 ≡ kwhX100。
// 时间或电量不可用时整单计入 unalloc 并标 QUALITY_UNAVAILABLE（不强行分摊，
// 08 文档第 3.2 节第 3 档），完整度会因此下降 —— 这正是它该有的效果。
SplitResult splitOrder(const QDateTime &start, const QDateTime &end,
                       qint64 kwhX100, const TariffPlan &plan);

// 排放量：(kwh_x100 * factor_g_per_kwh + 50) / 100，四舍五入到整数克。
// 入参越界返回 -1（调用方须判负），不做截断也不抛异常。
qint64 emissionG(qint64 kwhX100, qint64 factorGPerKwh);

// 排放强度 克/度：(emission_g * 100 + total/2) / total。total = 0 → NOT_APPLICABLE
qint64 intensityGPerKwh(qint64 emissionG, qint64 totalKwhX100);

// 完整度百分比：可分摊电量占比。total = 0 → NOT_APPLICABLE（不是 100）
int completenessPct(qint64 totalKwhX100, qint64 unallocKwhX100);

// 排放因子。生效区间左闭右开 [effectFrom, effectTo)，effectTo 为空串表示右开无穷。
struct Factor {
    int     factorId = 0;
    QString region;          // 描述性标签，不参与因子选择（见 factorOverlaps 说明）
    QString version;
    QString effectFrom;      // 'yyyy-MM-dd HH:mm:ss'
    QString effectTo;        // 空串 = 无穷
    qint64  gPerKwh = 0;
    QString source;
    bool    enabled = true;
};

// 因子生效边界必须对齐自然日 00:00:00。
//
// 为什么加这条约束：08 文档 5.3 要求「跨因子生效时间的订单必须切片后再计算」，
// 但 t_carbon_daily 的一行只记一个 factor_version（它还是 UNIQUE 键的一部分）。
// 若允许边界落在日内，同一天就会出现两个因子版本，一行装不下。
// 把边界钉在自然日上，按日选因子即**天然精确**，日内切片这件事就不存在了。
// 代价是因子不能在中午生效 —— 对一个按年发布的电网因子来说没有任何实际损失。
// 见 docs/expand/08-实现规划.md 裁决 D6。3742 写入前用它校验，不合法返回 ERR_PARAM。
bool isDayAlignedBoundary(const QString &timeText);

// 取在 atTime 生效的因子（左闭右开、只看 enabled）。无匹配返回 false。
bool pickFactor(const QVector<Factor> &factors, const QString &atTime, Factor *out);

// 新因子与既有因子的生效区间是否重叠。相邻不算重叠（[a,b) 与 [b,c) 合法）。
// 用于 3742 写入前校验，重叠返回 true → ERR_CARBON_FACTOR_OVERLAP(6702)。
// skipFactorId > 0 时跳过该条（修订自身时用）。
//
// ⚠ **跨区域也算重叠**：本系统只有一条全局因子时间线，pickFactor() 不看 region
//   （t_carbon_daily 与 t_station 都没有区域维度），region 只是描述性标签。
//   按区域分别查重叠的话，换个区域名就能绕过校验塞进重叠因子，历史数字会被悄悄改掉。
bool factorOverlaps(const QVector<Factor> &existing, const Factor &incoming,
                    int skipFactorId = 0);

// 找出将被新因子「接续」的开区间因子（effect_to 为空、且起点早于新因子起点）。
// 返回其 factor_id；没有返回 0；存在多个（本不该发生的破损状态）返回 -1。
//
// 为什么需要它：排放因子在现实中就是接续发布的 —— 2023 版一出，2022 版自然截止。
// 种子因子是 [2000-01-01, ∞)，若把它当成普通重叠一律拒绝，就再也发布不了新版本，
// 3742 出厂即不可用。所以新因子起点晚于开区间因子起点时，不算冲突，而是**接续**：
// 由 3742 在同一事务里把旧因子闭合到新因子的起点。
//
// ⚠ 接续会改变新起点之后所有日期的结果。这是发布新因子的应有之义，
//   但必须记日志，并且既有报告会在下次查询时被判为 STALE。
int openEndedPredecessor(const QVector<Factor> &existing, const Factor &incoming,
                         int skipFactorId = 0);

} // namespace carbon
} // namespace ecp
