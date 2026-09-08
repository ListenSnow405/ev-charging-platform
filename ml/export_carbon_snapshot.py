#!/usr/bin/env python3
"""
ml/export_carbon_snapshot.py  —  碳排放大屏快照导出　归属 L5

技术基线（CLAUDE.md 第 2 节「大屏取数」）：L5 只读 SQLite 导出 JSON，大屏轮询静态文件。
服务端因此保持纯 Socket，L2 不必额外写 HTTP 服务。

⚠ 只读 t_carbon_daily / t_carbon_factor / t_station，**不写任何表**，
  也不自己算分摊 —— 数字一律取服务端已经落库的聚合结果。
  这一条很重要：大屏若自己再算一遍，就会出现「大屏和管理端并排是两个形状」，
  而这正是 08 文档要求「同源」的原因。校验用的独立重算在 ml/carbon_crosscheck.py，
  那是对拍，不是取数。

⚠ 快照只包含 t_carbon_daily 里**已经算过**的日期。某个日期从没被查询过（懒聚合未触发）
  就不会出现在这里 —— 这不是 bug，先在管理端查一次或调 3745 即可。

用法：python3 ml/export_carbon_snapshot.py [charging.db] [dataviz/data/carbon.json]
"""
import json
import sqlite3
import sys
from datetime import datetime
from pathlib import Path

DB = Path(sys.argv[1] if len(sys.argv) > 1 else "charging.db")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "dataviz/data/carbon.json")

FMT = "%Y-%m-%d %H:%M:%S"
POLL_INTERVAL_SEC = 30
ALGO_VERSION = "carbon-v1"
TREND_DAYS = 30                       # 趋势图回看窗口
# 三处逐字一致：管理端页眉、服务端导出文件头、这里（实现规划第 2 节）
DISCLAIMER = "课程项目估算，非认证碳数据，不可用于碳交易或监管申报"


def pct_or_na(part, total):
    """总量为 0 时返回 -1「不适用」，不伪装成 0 或 100（00 第 4.5 节）。"""
    if total <= 0:
        return -1
    return (part * 100 + total // 2) // total


def main():
    if not DB.is_file():
        print(f"[FAIL] 找不到数据库 {DB}")
        return 1
    conn = sqlite3.connect(f"file:{DB.resolve()}?mode=ro", uri=True)

    try:
        rows = list(conn.execute(
            "SELECT stat_date, station_id, total_kwh_x100, peak_kwh_x100, flat_kwh_x100,"
            " valley_kwh_x100, unalloc_kwh_x100, order_cnt, emission_g, factor_version,"
            " cutoff_time FROM t_carbon_daily WHERE algo_version = ? ORDER BY stat_date",
            (ALGO_VERSION,)))
    except sqlite3.OperationalError as exc:
        print(f"[FAIL] 读取 t_carbon_daily 失败（先执行 docs/db-schema-ext-08.sql）: {exc}")
        return 1

    names = dict(conn.execute("SELECT station_id, name FROM t_station"))

    daily, by_station, versions = {}, {}, set()
    cutoff = ""
    for (d, sid, tot, pk, fl, va, un, cnt, em, fver, cut) in rows:
        versions.add(fver)
        if not cutoff or cut < cutoff:
            cutoff = cut                       # 取最保守的截止时刻，与服务端 3740 一致
        a = daily.setdefault(d, {"date": d, "total": 0, "peak": 0, "flat": 0,
                                 "valley": 0, "unalloc": 0, "orders": 0, "emission": 0})
        for k, v in (("total", tot), ("peak", pk), ("flat", fl), ("valley", va),
                     ("unalloc", un), ("orders", cnt), ("emission", em)):
            a[k] += v
        b = by_station.setdefault(sid, {"stationId": sid, "name": names.get(sid, f"站 {sid}"),
                                        "total": 0, "emission": 0})
        b["total"] += tot
        b["emission"] += em

    series = sorted(daily.values(), key=lambda r: r["date"])
    for r in series:
        r["completeness"] = pct_or_na(r["total"] - r["unalloc"], r["total"])
        # 强度 = 排放 ÷ 电量，整数四舍五入；总量为 0 → -1 不适用
        r["intensity"] = (r["emission"] * 100 + r["total"] // 2) // r["total"] if r["total"] else -1

    totals = {k: sum(r[k] for r in series)
              for k in ("total", "peak", "flat", "valley", "unalloc", "orders", "emission")}
    totals["completeness"] = pct_or_na(totals["total"] - totals["unalloc"], totals["total"])
    totals["intensity"] = ((totals["emission"] * 100 + totals["total"] // 2) // totals["total"]
                           if totals["total"] else -1)

    stations = sorted(by_station.values(), key=lambda r: -r["emission"])
    for r in stations:
        r["intensity"] = (r["emission"] * 100 + r["total"] // 2) // r["total"] if r["total"] else -1

    # 报告状态：大屏用它显示「有 N 份报告已过期」，与管理端的 STALE 角标同源
    try:
        reports = dict(conn.execute(
            "SELECT status, COUNT(*) FROM t_carbon_report GROUP BY status"))
    except sqlite3.OperationalError:
        reports = {}

    snapshot = {
        "generatedAt": datetime.now().strftime(FMT),
        "pollIntervalSec": POLL_INTERVAL_SEC,
        "disclaimer": DISCLAIMER,
        # 模块 05 分时电价未落地，大屏与管理端必须标同一个口径
        "tariffMode": "FIXED_RANGE",
        "tariffNote": "峰 10:00-15:00、18:00-21:00；谷 23:00-07:00；平 = 24h 扣除峰谷（左闭右开）",
        "factorVersions": sorted(versions),
        "algoVersion": ALGO_VERSION,
        "cutoffTime": cutoff,
        "totals": totals,
        "daily": series[-TREND_DAYS:],
        "dailyAll": series,
        "byStation": stations,
        "reportStatus": reports,
        "unit": {"kwh": "kwh_x100（度 ×100 整数）", "emission": "emission_g（整数克）"},
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(snapshot, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"已导出 {OUT}：{len(series)} 天，{len(stations)} 站，"
          f"电量 {totals['total']/100:.2f} 度，排放 {totals['emission']/1000:.2f} kg，"
          f"完整度 {totals['completeness']}%")
    if not series:
        print("[注意] t_carbon_daily 为空 —— 先在管理端查一次碳排放报告，或调 3745 重算")
    return 0


if __name__ == "__main__":
    sys.exit(main())
