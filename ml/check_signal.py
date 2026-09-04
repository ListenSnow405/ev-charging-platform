#!/usr/bin/env python3
"""
ml/check_signal.py  —  时序信号体检　归属 L5

用途：量化 t_order 里到底有没有「可学的时序结构」，作为 gen_history.py 改造的验收闸。

背景：改造前实测 t−24h 自相关仅 0.055、t−168h 0.043——六个站共用同一条 HOUR_WEIGHTS，
日与日之间天气独立抽样，结构上就不存在跨日相关。在这种数据上做特征工程，
滞后特征全是噪声，模型唯一能学到的只有写死的那条常量曲线；
而 [说明书] 1.4 要求预测「各站点」负荷，「分站」在这份数据里同样是空的。

三条验收线（L5-PLAN 阶段 1）：
    1. t−24h 自相关 ≥ 0.30
    2. 六站日内曲线可分（平均两两 L1 距离 ≥ 0.30）
    3. 日内峰谷比 ≥ 5.0        ← 防回归：加 AR(1) 与站点画像时很容易把峰谷抹平

⚠ 运行期脚本，业务表一律 mode=ro 只读连接（ml/CLAUDE.md 数据库权限）。

用法：
    python3 ml/check_signal.py                 # 默认体检 ml/data/dev.db
    python3 ml/check_signal.py charging.db
"""
import argparse
import math
import sqlite3
import sys
from datetime import datetime, timedelta
from pathlib import Path

DB_DEFAULT = Path("ml/data/dev.db")
FMT = "%Y-%m-%d %H:%M:%S"

# 验收阈值。改这里等于改验收标准，需在 L5-PLAN 同步。
TH_LAG24 = 0.30
# 可分性的噪声底实测 0.149——把六个站强制成同一画像重新生成，读数就是这个值，
# 纯粹来自抽样噪声。0.30 取它的两倍，才算「真的分开了」而不是噪声。
# （改造前订单量只有现在的 1/4，同样口径的噪声底高达 0.262，那时这个阈值贴着噪声，
#   没有区分力——阈值必须跟着数据量一起校准，不能拍脑袋定死。）
TH_SEPARABILITY = 0.30
TH_PEAK_VALLEY = 5.0

SPARK = "▁▂▃▄▅▆▇█"


def parse_args():
    ap = argparse.ArgumentParser(description="检查历史数据的时序信号强度，gen_history.py 改造的验收闸")
    ap.add_argument("db", nargs="?", default=str(DB_DEFAULT))
    return ap.parse_args()


def pearson(xs, ys):
    n = len(xs)
    if n < 2:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def autocorr(series, lag):
    if len(series) <= lag:
        return float("nan")
    return pearson(series[lag:], series[:-lag])


def build_panel(con):
    """站-小时负荷面板。每个会话的电量按重叠时长摊到各小时桶，桶内能量即该小时的平均 kW。

    只取 status=3（已结算）：0/1/2 在历史区间里本就不该存在，4（已取消）没有电量。
    """
    stations = dict(con.execute("SELECT station_id, name FROM t_station").fetchall())
    rows = con.execute(
        "SELECT station_id, start_time, end_time, kwh_x100 FROM t_order "
        "WHERE status = 3 AND start_time IS NOT NULL AND end_time IS NOT NULL"
    ).fetchall()
    if not rows:
        print("t_order 里没有已结算订单，先跑 gen_history.py 播种", file=sys.stderr)
        sys.exit(1)

    buckets = {}          # (station_id, 小时起点 datetime) -> kWh
    t_min, t_max = None, None
    for sid, st, et, kwh_x100 in rows:
        s = datetime.strptime(st, FMT)
        e = datetime.strptime(et, FMT)
        total_s = (e - s).total_seconds()
        if total_s <= 0:
            continue
        kwh = kwh_x100 / 100.0
        t_min = s if t_min is None or s < t_min else t_min
        t_max = e if t_max is None or e > t_max else t_max
        cur = s.replace(minute=0, second=0, microsecond=0)
        while cur < e:
            nxt = cur + timedelta(hours=1)
            overlap = (min(e, nxt) - max(s, cur)).total_seconds()
            if overlap > 0:
                buckets[(sid, cur)] = buckets.get((sid, cur), 0.0) + kwh * overlap / total_s
            cur = nxt

    # 补零成连续小时轴——缺的小时是「负荷为 0」而不是「没有观测」，
    # 漏补会让自相关只在有单的小时上算，把静默时段的规律性丢掉。
    h0 = t_min.replace(minute=0, second=0, microsecond=0)
    h1 = t_max.replace(minute=0, second=0, microsecond=0)
    hours = []
    cur = h0
    while cur <= h1:
        hours.append(cur)
        cur += timedelta(hours=1)

    panel = {sid: [buckets.get((sid, h), 0.0) for h in hours] for sid in stations}
    return stations, hours, panel


def deseasonalize(hours, series):
    """减去 (工作日/周末 × 小时) 的均值，剩下的才是日历解释不了的部分。

    原始 lag-24 自相关里混着日内周期本身——只要有昼夜曲线它就不会低。
    去季节后仍然显著，才说明存在 AR(1) 那种「忙日成簇」的跨日持续性，
    也才说明阶段 3 的滞后特征真能跑赢「历史同小时均值」基线。
    """
    acc = {}
    for h, v in zip(hours, series):
        key = (h.weekday() >= 5, h.hour)
        a = acc.setdefault(key, [0.0, 0])
        a[0] += v
        a[1] += 1
    return [v - acc[(h.weekday() >= 5, h.hour)][0] / acc[(h.weekday() >= 5, h.hour)][1]
            for h, v in zip(hours, series)]


def hour_profile(hours, series):
    """归一化日内曲线：24 个小时各占全天负荷的比例，和为 1。

    归一化是为了让「形状」可比——不归一化的话，桩多的站单纯因为量大就显得不一样，
    那不是曲线可分，是规模可分。
    """
    acc = [0.0] * 24
    for h, v in zip(hours, series):
        acc[h.hour] += v
    tot = sum(acc)
    return [a / tot for a in acc] if tot > 0 else acc


def spark(profile):
    top = max(profile) or 1.0
    return "".join(SPARK[min(len(SPARK) - 1, int(v / top * (len(SPARK) - 1) + 0.5))] for v in profile)


def main() -> int:
    args = parse_args()
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"数据库不存在：{db_path}", file=sys.stderr)
        return 1

    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    stations, hours, panel = build_panel(con)
    con.close()

    print(f"体检对象：{db_path}")
    print(f"小时轴：{hours[0]:%Y-%m-%d %H} ~ {hours[-1]:%Y-%m-%d %H}（{len(hours)} 小时，{len(stations)} 站）\n")

    # ---- 1. 自相关 ----------------------------------------------------------
    print("【1】自相关（各站分别计算后取均值）")
    print(f"{'站点':<16}{'t−1h':>9}{'t−24h':>9}{'t−168h':>9}   去季节 t−24h")
    lag24s, ds24s = [], []
    for sid, name in sorted(stations.items()):
        s = panel[sid]
        a1, a24, a168 = autocorr(s, 1), autocorr(s, 24), autocorr(s, 168)
        d24 = autocorr(deseasonalize(hours, s), 24)
        lag24s.append(a24)
        ds24s.append(d24)
        print(f"{name:<16}{a1:>9.3f}{a24:>9.3f}{a168:>9.3f}{d24:>15.3f}")
    mean_lag24 = sum(lag24s) / len(lag24s)
    mean_ds24 = sum(ds24s) / len(ds24s)
    total = [sum(panel[sid][i] for sid in stations) for i in range(len(hours))]
    print(f"\n  站均 t−24h = {mean_lag24:.3f}　去季节站均 t−24h = {mean_ds24:.3f}")
    print(f"  全网合计序列 t−24h = {autocorr(total, 24):.3f}"
          f"　t−168h = {autocorr(total, 168):.3f}\n")

    # ---- 2. 分站可分性 ------------------------------------------------------
    print("【2】日内曲线（归一化占比，0~23 时）")
    profiles = {sid: hour_profile(hours, panel[sid]) for sid in stations}
    for sid, name in sorted(stations.items()):
        print(f"  {name:<16}{spark(profiles[sid])}")
    sids = sorted(stations)
    dists = [sum(abs(a - b) for a, b in zip(profiles[i], profiles[j]))
             for x, i in enumerate(sids) for j in sids[x + 1:]]
    mean_dist, min_dist, max_dist = sum(dists) / len(dists), min(dists), max(dists)
    print(f"\n  两两 L1 距离：均值 {mean_dist:.3f}　最小 {min_dist:.3f}　最大 {max_dist:.3f}"
          f"（{len(dists)} 对，取值范围 0~2）\n")

    # ---- 3. 峰谷比 ----------------------------------------------------------
    tp = hour_profile(hours, total)
    peak_h, valley_h = tp.index(max(tp)), tp.index(min(tp))
    ratio = max(tp) / min(tp) if min(tp) > 0 else float("inf")
    print("【3】全网日内峰谷比")
    print(f"  峰 {peak_h:02d}:00（{max(tp)*100:.1f}%）　谷 {valley_h:02d}:00（{min(tp)*100:.1f}%）"
          f"　峰谷比 {ratio:.1f}x\n")

    # ---- 4. 物理合理性 ------------------------------------------------------
    # 提高订单密度是为了压过泊松噪声，但站点装机功率是硬上界：小时负荷画到装机的
    # 两倍，大屏的负荷曲线在答辩现场就是个明显破绽。而且 t_load_forecast 要预测的
    # idle_pile / congestion 以「桩被占用」为语义前提，没有占用约束时它们只能靠
    # clamp 兜底，预测的是个假量。这一项防的就是「为了刷相关系数把站充爆」。
    print("【4】物理合理性（站点小时负荷 vs 装机功率）")
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    installed = {}
    for sid, power, cnt in con.execute(
            "SELECT station_id, SUM(power), COUNT(*) FROM t_pile GROUP BY station_id"):
        installed[sid] = (power, cnt)
    con.close()
    worst = 0.0
    for sid, name in sorted(stations.items()):
        cap = installed.get(sid, (0.0, 0))[0]
        peak = max(panel[sid]) if panel[sid] else 0.0
        pct = peak / cap * 100 if cap else float("inf")
        worst = max(worst, pct)
        print(f"  {name:<16}峰值 {peak:6.1f} kW / 装机 {cap:6.1f} kW = {pct:5.1f}%")
    print()

    # ---- 判定 ---------------------------------------------------------------
    checks = [
        ("t−24h 自相关", mean_lag24, TH_LAG24, "≥"),
        ("分站曲线可分性", mean_dist, TH_SEPARABILITY, "≥"),
        ("日内峰谷比", ratio, TH_PEAK_VALLEY, "≥"),
    ]
    print("验收：")
    ok_load = worst <= 100.0
    print(f"  [{'PASS' if ok_load else 'FAIL'}] {'峰值负荷占装机':<13}{worst:>6.1f}%  （阈值 ≤ 100%，超过即物理不可能）")
    failed = 0 if ok_load else 1
    for label, value, th, _ in checks:
        ok = value >= th
        failed += 0 if ok else 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {label:<14}{value:>7.3f}  （阈值 ≥ {th}）")
    print()
    if failed:
        print(f"{failed} 项未达标——数据尚不足以支撑分站时序建模。")
        return 1
    print("四项全部达标，可以进入阶段 2 特征工程。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
