// -----------------------------------------------------------------------------
//  server/biz/ext_08_carbon_calc_test.cpp  —  模块 08 计算核心的手算夹具
//  归属 L5。运行：bash scripts/test-carbon-calc.sh
//
//  ⚠ 本文件**不在 server.pro 的 SOURCES 里**，不进服务端产物，只由测试脚本单独编译。
//
//  这里的每个期望值都是**人手算出来的**，不是把代码跑一遍再抄回来的。
//  08 文档第 2 节要求「先用一组能手算的小数据跑通闭环，再做图表」——
//  夹具一旦改成「以实现为准」，这层验证就自动失效了，改期望值前请先在纸上重算。
//
//  时段口径（docs/db-schema-ext-08.sql 种子）：
//    峰 10:00-15:00、18:00-21:00      共 8h
//    谷 23:00-07:00（跨午夜）          共 8h
//    平 = 24h 扣除峰谷 → 07-10、15-18、21-23   共 8h
//  演示因子 581 g/kWh
// -----------------------------------------------------------------------------
#include <QDateTime>
#include <QTextStream>
#include <QVector>
#include <cstdio>

#include "ext_08_carbon_calc.h"

using namespace ecp::carbon;

static int g_pass = 0, g_fail = 0;
static QTextStream out(stdout);

static void check(bool ok, const QString &name, const QString &detail = QString())
{
    if (ok) { ++g_pass; out << "  [PASS] " << name << "\n"; }
    else    { ++g_fail; out << "  [FAIL] " << name << (detail.isEmpty() ? "" : "  → " + detail) << "\n"; }
    out.flush();
}

static void eq(qint64 got, qint64 want, const QString &name)
{
    check(got == want, name, QStringLiteral("期望 %1，实得 %2").arg(want).arg(got));
}

static QDateTime dt(const QString &s)
{
    return QDateTime::fromString(s, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// 一条分摊用例：期望值全部手算
static void splitCase(const TariffPlan &plan, const QString &name,
                      const QString &start, const QString &end, qint64 kwh,
                      qint64 wPeak, qint64 wFlat, qint64 wValley, qint64 wUnalloc,
                      Quality wQuality)
{
    const SplitResult r = splitOrder(start.isEmpty() ? QDateTime() : dt(start),
                                     end.isEmpty()   ? QDateTime() : dt(end), kwh, plan);
    const bool ok = r.peak == wPeak && r.flat == wFlat && r.valley == wValley
                 && r.unalloc == wUnalloc && r.quality == wQuality;
    check(ok, name, QStringLiteral("期望 峰%1 平%2 谷%3 未分配%4 %5，实得 峰%6 平%7 谷%8 未分配%9 %10")
              .arg(wPeak).arg(wFlat).arg(wValley).arg(wUnalloc).arg(qualityName(wQuality))
              .arg(r.peak).arg(r.flat).arg(r.valley).arg(r.unalloc).arg(qualityName(r.quality)));
    // 守恒是不变量，每条用例都顺带验一次（负电量等非法输入除外，它们整单不计）
    if (kwh >= 0 && kwh <= KWH_X100_MAX)
        eq(r.total(), kwh, name + QStringLiteral(" · 四项之和 = 总量"));
}

int main()
{
    out << "\n模块 08 碳排放计算核心 · 手算夹具\n";
    out << "================================================================\n";

    // ---- 1. 时段表解析 -------------------------------------------------------
    out << "\n[1] 时段表解析\n";
    const TariffPlan plan = parseTariffPlan(QStringLiteral("10:00-15:00,18:00-21:00"),
                                            QStringLiteral("23:00-07:00"));
    check(plan.valid, QStringLiteral("标准配置解析通过"), plan.error);
    eq(plan.peak.size(), 2, QStringLiteral("峰段 2 片"));
    // 23:00-07:00 跨午夜，解析时应就地拆成 [23:00,24:00) 与 [00:00,07:00) 两片
    eq(plan.valley.size(), 2, QStringLiteral("谷段跨午夜拆成 2 片"));
    eq(plan.valley[0].fromSec, 82800, QStringLiteral("谷段片1 起点 = 23:00 = 82800s"));
    eq(plan.valley[0].toSec,   86400, QStringLiteral("谷段片1 终点 = 24:00 = 86400s"));
    eq(plan.valley[1].fromSec, 0,     QStringLiteral("谷段片2 起点 = 00:00"));
    eq(plan.valley[1].toSec,   25200, QStringLiteral("谷段片2 终点 = 07:00 = 25200s"));

    check(!parseTariffPlan(QStringLiteral("10:00-15:00"), QStringLiteral("12:00-13:00")).valid,
          QStringLiteral("峰谷重叠 → 判非法"));
    check(!parseTariffPlan(QStringLiteral("10:00-15:00,14:00-16:00"), QStringLiteral("23:00-07:00")).valid,
          QStringLiteral("峰段自身重叠 → 判非法"));
    check(!parseTariffPlan(QStringLiteral("abc"), QStringLiteral("23:00-07:00")).valid,
          QStringLiteral("格式错误 → 判非法"));
    check(!parseTariffPlan(QStringLiteral("10:00-10:00"), QStringLiteral("23:00-07:00")).valid,
          QStringLiteral("零长度时段 → 判非法"));

    // ---- 2. 分时分摊（手算） -------------------------------------------------
    out << "\n[2] 分时分摊 —— 期望值手算\n";
    out << "    手算过程：\n";
    out << "    A 11:00-13:00 全落峰段(10-15)，2h/2h → 峰 1000\n";
    out << "    B 09:00-11:00 平 1h + 峰 1h，1200×(3600/7200)=600 各半\n";
    out << "    C 22:00-次日02:00 平(22-23)1h + 谷(23-07)3h，1000×3600/14400=250 / ×10800/14400=750\n";
    out << "    D 08:00-11:00 平 2h + 峰 1h，100 单位除不尽：\n";
    out << "      峰 100×3600/10800=33 余3600  平 100×7200/10800=66 余7200 → 余数大的平段补 1 → 67\n";

    splitCase(plan, QStringLiteral("A 全落峰段"),
              QStringLiteral("2026-07-01 11:00:00"), QStringLiteral("2026-07-01 13:00:00"),
              1000, 1000, 0, 0, 0, QUALITY_UNIFORM_ESTIMATE);

    splitCase(plan, QStringLiteral("B 跨平峰边界"),
              QStringLiteral("2026-07-01 09:00:00"), QStringLiteral("2026-07-01 11:00:00"),
              1200, 600, 600, 0, 0, QUALITY_UNIFORM_ESTIMATE);

    splitCase(plan, QStringLiteral("C 跨午夜 + 跨平谷"),
              QStringLiteral("2026-07-01 22:00:00"), QStringLiteral("2026-07-02 02:00:00"),
              1000, 0, 250, 750, 0, QUALITY_UNIFORM_ESTIMATE);

    splitCase(plan, QStringLiteral("D 除不尽 → 最大余数法"),
              QStringLiteral("2026-07-01 08:00:00"), QStringLiteral("2026-07-01 11:00:00"),
              100, 33, 67, 0, 0, QUALITY_UNIFORM_ESTIMATE);

    // 整日：峰 8h / 平 8h / 谷 8h，2400 单位应恰好三等分
    splitCase(plan, QStringLiteral("E 整个自然日 → 峰平谷各 1/3"),
              QStringLiteral("2026-07-01 00:00:00"), QStringLiteral("2026-07-02 00:00:00"),
              2400, 800, 800, 800, 0, QUALITY_UNIFORM_ESTIMATE);

    // ---- 3. 降级与非法输入 ---------------------------------------------------
    out << "\n[3] 降级与非法输入\n";
    splitCase(plan, QStringLiteral("缺 end_time → 整单未分配"),
              QStringLiteral("2026-07-01 11:00:00"), QString(),
              1000, 0, 0, 0, 1000, QUALITY_UNAVAILABLE);

    splitCase(plan, QStringLiteral("end <= start → 整单未分配"),
              QStringLiteral("2026-07-01 11:00:00"), QStringLiteral("2026-07-01 11:00:00"),
              1000, 0, 0, 0, 1000, QUALITY_UNAVAILABLE);

    splitCase(plan, QStringLiteral("零电量 + 时间合法 → 四项全零，仍可分摊"),
              QStringLiteral("2026-07-01 11:00:00"), QStringLiteral("2026-07-01 13:00:00"),
              0, 0, 0, 0, 0, QUALITY_UNIFORM_ESTIMATE);

    splitCase(plan, QStringLiteral("会话超 7 天 → 整单未分配"),
              QStringLiteral("2026-07-01 11:00:00"), QStringLiteral("2026-07-09 11:00:00"),
              1000, 0, 0, 0, 1000, QUALITY_UNAVAILABLE);

    {   // 负电量：连 unalloc 都不填，否则守恒断言会变成脏数据的掩护
        const SplitResult r = splitOrder(dt(QStringLiteral("2026-07-01 11:00:00")),
                                         dt(QStringLiteral("2026-07-01 13:00:00")), -500, plan);
        check(r.total() == 0 && r.quality == QUALITY_UNAVAILABLE,
              QStringLiteral("负电量 → 全零 + UNAVAILABLE，不计入任何项"));
    }
    {   // 上限边界：恰好 KWH_X100_MAX 应正常分摊，+1 则整单拒绝
        const SplitResult okR = splitOrder(dt(QStringLiteral("2026-07-01 11:00:00")),
                                           dt(QStringLiteral("2026-07-01 13:00:00")), KWH_X100_MAX, plan);
        eq(okR.peak, KWH_X100_MAX, QStringLiteral("电量上限 1e11 → 正常分摊，中间乘法不溢出"));
        const SplitResult bad = splitOrder(dt(QStringLiteral("2026-07-01 11:00:00")),
                                           dt(QStringLiteral("2026-07-01 13:00:00")), KWH_X100_MAX + 1, plan);
        check(bad.total() == 0 && bad.quality == QUALITY_UNAVAILABLE,
              QStringLiteral("电量超上限 → 整单拒绝"));
    }

    // ---- 4. 守恒模糊测试 -----------------------------------------------------
    out << "\n[4] 守恒模糊测试（确定性伪随机 5000 例）\n";
    {
        quint64 seed = 20260908ULL;      // 固定种子：失败可复现，不是每次换一批数据碰运气
        auto next = [&seed]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                                return static_cast<quint64>(seed >> 33); };
        int bad = 0;
        for (int i = 0; i < 5000; ++i) {
            const QDateTime s = dt(QStringLiteral("2026-07-01 00:00:00")).addSecs(next() % (86400 * 3));
            const QDateTime e = s.addSecs(60 + next() % (86400 * 2));       // 1 分钟 ~ 2 天
            const qint64 kwh  = static_cast<qint64>(next() % 100000);
            const SplitResult r = splitOrder(s, e, kwh, plan);
            if (r.total() != kwh) ++bad;
        }
        eq(bad, 0, QStringLiteral("5000 例四项之和恒等于总量"));
    }

    // ---- 5. 排放与派生指标（手算） -------------------------------------------
    out << "\n[5] 排放与派生指标 —— 期望值手算\n";
    out << "    手算过程：\n";
    out << "    10 度 × 581 = 5810 g 整\n";
    out << "    0.50 度 × 581 = 290.5 g → 四舍五入 291（整数写法 (50×581+50)/100 = 291）\n";
    out << "    强度 = 5810 g ÷ 10 度 = 581 g/度，与因子相等（自洽性检查）\n";
    eq(emissionG(1000, 581), 5810, QStringLiteral("10 度 → 5810 g"));
    eq(emissionG(50, 581),   291,  QStringLiteral("0.50 度 → 290.5 四舍五入为 291"));
    eq(emissionG(1, 581),    6,    QStringLiteral("0.01 度 → 5.81 四舍五入为 6"));
    eq(emissionG(0, 581),    0,    QStringLiteral("零电量 → 0 g"));
    eq(emissionG(-1, 581),   -1,   QStringLiteral("负电量 → -1 拒绝"));
    eq(emissionG(1000, 0),   -1,   QStringLiteral("因子非正 → -1 拒绝"));
    eq(emissionG(KWH_X100_MAX, 581), 581000000000LL,
       QStringLiteral("上限电量 1e11 × 581 → 5.81e11 g，qint64 不溢出"));
    eq(emissionG(KWH_X100_MAX + 1, 581), -1, QStringLiteral("超上限 → -1 拒绝"));

    eq(intensityGPerKwh(5810, 1000), 581, QStringLiteral("强度 5810g / 10度 = 581 g/度"));
    eq(intensityGPerKwh(5810, 0),  NOT_APPLICABLE, QStringLiteral("总量为 0 → 不适用(-1)，不是 0"));
    eq(intensityGPerKwh(-1, 1000), NOT_APPLICABLE, QStringLiteral("排放非法 → 不适用(-1)"));

    eq(completenessPct(1000, 250),  75,  QStringLiteral("完整度 750/1000 = 75%"));
    eq(completenessPct(1000, 0),    100, QStringLiteral("完整度 全部可分摊 = 100%"));
    eq(completenessPct(1000, 1000), 0,   QStringLiteral("完整度 全部不可分摊 = 0%"));
    eq(completenessPct(3, 1),       67,  QStringLiteral("完整度 2/3 = 66.67 四舍五入为 67"));
    eq(completenessPct(0, 0), NOT_APPLICABLE,
       QStringLiteral("总量为 0 → 不适用(-1)，**不伪装成 100**"));

    // ---- 6. 因子选择与重叠 ---------------------------------------------------
    out << "\n[6] 因子生效区间 —— 左闭右开 [from, to)\n";
    QVector<Factor> factors;
    Factor a; a.factorId = 1; a.region = QStringLiteral("华南"); a.version = QStringLiteral("v1");
    a.effectFrom = QStringLiteral("2026-01-01 00:00:00");
    a.effectTo   = QStringLiteral("2026-07-01 00:00:00");
    a.gPerKwh = 600; factors.append(a);
    Factor b; b.factorId = 2; b.region = QStringLiteral("华南"); b.version = QStringLiteral("v2");
    b.effectFrom = QStringLiteral("2026-07-01 00:00:00");
    b.effectTo   = QString();                       // 右开无穷
    b.gPerKwh = 581; factors.append(b);

    Factor got;
    check(pickFactor(factors, QStringLiteral("2026-01-01 00:00:00"), &got) && got.factorId == 1,
          QStringLiteral("起点当刻命中（左闭）"));
    check(pickFactor(factors, QStringLiteral("2026-06-30 23:59:59"), &got) && got.factorId == 1,
          QStringLiteral("终点前一秒仍命中 v1"));
    check(pickFactor(factors, QStringLiteral("2026-07-01 00:00:00"), &got) && got.factorId == 2,
          QStringLiteral("终点当刻切换到 v2（右开）"));
    check(!pickFactor(factors, QStringLiteral("2025-12-31 23:59:59"), &got),
          QStringLiteral("生效前无因子 → 未命中（服务端应返回 6701）"));
    {
        QVector<Factor> disabled = factors;
        disabled[1].enabled = false;
        check(!pickFactor(disabled, QStringLiteral("2026-08-01 00:00:00"), &got),
              QStringLiteral("停用的因子不参与选择"));
    }

    out << "\n[7] 因子区间重叠检测（3742 写入前校验）\n";
    Factor n;  n.region = QStringLiteral("华南");
    n.effectFrom = QStringLiteral("2026-06-01 00:00:00"); n.effectTo = QString();
    check(factorOverlaps(factors, n), QStringLiteral("与 v1 区间相交 → 判重叠(6702)"));
    n.effectFrom = QStringLiteral("2025-01-01 00:00:00"); n.effectTo = QStringLiteral("2026-01-01 00:00:00");
    check(!factorOverlaps(factors, n), QStringLiteral("[a,b) 紧邻 [b,c) → 不算重叠"));
    n.effectFrom = QStringLiteral("2026-06-01 00:00:00"); n.effectTo = QString();
    n.region = QStringLiteral("华东");
    check(!factorOverlaps(factors, n), QStringLiteral("不同区域 → 不比较"));
    n.region = QStringLiteral("华南");
    n.effectFrom = QStringLiteral("2026-01-01 00:00:00");
    n.effectTo   = QStringLiteral("2026-07-01 00:00:00");
    check(!factorOverlaps(factors, n, /*skipFactorId=*/1),
          QStringLiteral("修订自身时跳过自己 → 不判重叠"));

    out << "\n[8] 因子边界必须对齐自然日（裁决 D6）\n";
    check(isDayAlignedBoundary(QStringLiteral("2026-01-01 00:00:00")),
          QStringLiteral("00:00:00 → 合法"));
    check(!isDayAlignedBoundary(QStringLiteral("2026-01-01 12:00:00")),
          QStringLiteral("日内时刻 → 拒绝（否则一天会横跨两个因子版本）"));
    check(!isDayAlignedBoundary(QStringLiteral("not-a-time")),
          QStringLiteral("格式非法 → 拒绝"));

    out << "\n================================================================\n";
    out << QStringLiteral("通过 %1 / 失败 %2 → %3\n").arg(g_pass).arg(g_fail)
             .arg(g_fail == 0 ? QStringLiteral("RESULT: PASS") : QStringLiteral("RESULT: FAIL"));
    out.flush();
    return g_fail == 0 ? 0 : 1;
}
