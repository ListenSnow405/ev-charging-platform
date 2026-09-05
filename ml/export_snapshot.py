#!/usr/bin/env python3
"""
ml/export_snapshot.py  —  大屏数据快照导出　归属 L5

技术基线（CLAUDE.md 第 2 节「大屏取数」）：
    L5 只读 SQLite，定时导出 JSON 快照，大屏轮询静态文件。
    服务端因此保持纯 Socket，L2 不必额外写 HTTP 服务。

⚠ 只读，禁止写业务表。预测结果由 ml/predict.py 写 t_load_forecast，这里只读回来。

快照分两层：
  实时层  营收三指标 / 近 7、30 日趋势 / 电桩状态 / 站点排行 / 近 48 小时负荷曲线 / 用户行为
  预测层  t_load_forecast 最新一批（6 站 × 1、6、24 小时），与实时负荷曲线画在同一张图上

轮询间隔写在快照 JSON 里（`pollIntervalSec`），**不放 t_sys_config**：
那张表归 L2，且 ml/CLAUDE.md 明确禁止本模块写它；而轮询间隔纯粹是大屏与导出器之间的约定，
两端都归 L5，放 JSON 里既不动冻结契约，也让前端不必额外读一次库。

用法：python3 ml/export_snapshot.py [charging.db] [dataviz/data/snapshot.json]
"""
import json
import sqlite3
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

DB = Path(sys.argv[1] if len(sys.argv) > 1 else "charging.db")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "dataviz/data/snapshot.json")

FMT = "%Y-%m-%d %H:%M:%S"
POLL_INTERVAL_SEC = 30        # 大屏轮询间隔，见文件头说明
LOAD_CURVE_HOURS = 48         # 负荷曲线回看窗口
BEHAVIOR_DAYS = 30            # 用户行为统计窗口

# 说明书 1.4 管理端/大屏指标，SQL 与 docs/db-schema.sql 末尾的参考查询一致
# 口径必须与服务端 2302 (server/biz/statistics_service.cpp) 完全一致：
# 管理端和大屏展示同一个指标，各算各的会在答辩现场并排显示成两个形状。
# 服务端取 [today-(days-1), today] 共 days 个完整自然日，并对无订单的日期补零。
# 原先写的 datetime('now','-7 day') 是从此刻往前推 7×24 小时，会多带出半天，
# 首日柱子矮一截；且不补零会让折线图的日期轴断掉。
SQL_TREND = """
SELECT date(settle_time) AS d, IFNULL(SUM(amount),0) AS amt
  FROM t_order WHERE status = 3
   AND date(settle_time) >= ? AND date(settle_time) <= ?
 GROUP BY d ORDER BY d"""
# 同理与 2303 对齐：用 SUM(CASE) 保证三种状态都有值，
# GROUP BY 在某状态零台时会整个缺项，饼图会少一块。
SQL_STATUS = """
SELECT
  IFNULL(SUM(CASE WHEN status = 0 THEN 1 ELSE 0 END),0),
  IFNULL(SUM(CASE WHEN status = 1 THEN 1 ELSE 0 END),0),
  IFNULL(SUM(CASE WHEN status = 2 THEN 1 ELSE 0 END),0)
FROM t_pile"""
SQL_RANK = """
SELECT s.name, IFNULL(SUM(o.amount),0) AS amt
  FROM t_station s LEFT JOIN t_order o
    ON o.station_id = s.station_id AND o.status = 3
 GROUP BY s.station_id ORDER BY amt DESC"""
# 近 N 小时的会话，用于在 Python 侧把电量按重叠时长摊进小时桶。
# 不在 SQL 里做：一个会话可能跨多个小时桶，SQL 摊不动，硬凑会把跨小时的慢充算错。
SQL_SESSIONS = """
SELECT start_time, end_time, kwh_x100 FROM t_order
 WHERE status = 3 AND start_time IS NOT NULL AND end_time IS NOT NULL
   AND end_time >= ?"""
# 预测层：协议 2305 规定「取同一 model_version 下 create_time 最大的一批」，这里同口径。
SQL_FORECAST = """
SELECT f.station_id, s.name, f.horizon, f.predict_time, f.load_kw,
       f.idle_pile, f.is_peak, f.congestion, f.model_version, f.create_time
  FROM t_load_forecast f JOIN t_station s ON s.station_id = f.station_id
 WHERE f.create_time = (SELECT MAX(create_time) FROM t_load_forecast
                         WHERE model_version = f.model_version)
 ORDER BY f.horizon, f.station_id"""
SQL_BEHAVIOR_HOUR = """
SELECT CAST(strftime('%H', reserve_time) AS INTEGER) AS h, COUNT(*)
  FROM t_order WHERE date(reserve_time) >= ? GROUP BY h"""
SQL_BEHAVIOR_TYPE = """
SELECT p.type, COUNT(*) FROM t_order o JOIN t_pile p ON p.pile_id = o.pile_id
 WHERE o.status = 3 AND date(o.reserve_time) >= ? GROUP BY p.type"""
SQL_BEHAVIOR_STATUS = """
SELECT o.status, COUNT(*) FROM t_order o
 WHERE date(o.reserve_time) >= ? GROUP BY o.status"""
SQL_REVENUE = """
SELECT
  IFNULL(SUM(CASE WHEN date(settle_time)=date('now','localtime')      THEN amount END),0),
  IFNULL(SUM(CASE WHEN strftime('%Y-%m',settle_time)=strftime('%Y-%m','now','localtime') THEN amount END),0),
  IFNULL(SUM(amount),0)
FROM t_order WHERE status = 3"""

def trend(cur, days: int, end_d: date):
    """近 N 日营收趋势，含补零。

    口径必须与服务端 2302 完全一致：取 [today-(days-1), today] 共 days 个完整自然日，
    并对无订单的日期补零。管理端和大屏展示同一个指标，各算各的会在答辩现场
    并排显示成两个形状。
    """
    start_d = end_d - timedelta(days=days - 1)
    got = dict(cur.execute(SQL_TREND, (start_d.isoformat(), end_d.isoformat())).fetchall())
    return [{"date": (start_d + timedelta(days=i)).isoformat(),
             "amount": got.get((start_d + timedelta(days=i)).isoformat(), 0)}
            for i in range(days)]


def load_curve(cur, hours: int):
    """近 N 小时全网负荷曲线：每个会话的电量按重叠时长摊到各小时桶。

    桶内能量数值上即该小时的平均功率（kW）。与 ml/build_features.py 的 load_kw 同口径，
    否则实时曲线和预测点画在同一张图上会是两个量纲。

    窗口右端锚在**最后一个有观测的小时**，不是「现在」。锚在现在的话，
    观测与当前时刻之间的空档会被画成一条掉到 0 的实线，而预测虚线在同一段上却有值，
    同一张图上自相矛盾。锚在观测末尾还有一个好处：它与 ml/predict.py 取的 origin
    是同一个时刻，实线的终点正好接上虚线的起点。
    """
    last = cur.execute(
        "SELECT MAX(end_time) FROM t_order WHERE status = 3 AND end_time IS NOT NULL"
    ).fetchone()[0]
    if not last:
        return [], []
    end_h = datetime.strptime(last, FMT).replace(minute=0, second=0, microsecond=0)
    start = end_h - timedelta(hours=hours - 1)
    buckets = {}
    for st, et, kwh_x100 in cur.execute(SQL_SESSIONS, (start.strftime(FMT),)):
        s_dt, e_dt = datetime.strptime(st, FMT), datetime.strptime(et, FMT)
        total = (e_dt - s_dt).total_seconds()
        if total <= 0:
            continue
        cur_h = s_dt.replace(minute=0, second=0, microsecond=0)
        while cur_h < e_dt:
            nxt = cur_h + timedelta(hours=1)
            ov = (min(e_dt, nxt) - max(s_dt, cur_h)).total_seconds()
            if ov > 0 and cur_h >= start:
                buckets[cur_h] = buckets.get(cur_h, 0.0) + kwh_x100 / 100.0 * ov / total
            cur_h = nxt
    axis = [start + timedelta(hours=i) for i in range(hours)]
    return ([h.strftime(FMT) for h in axis],
            [round(buckets.get(h, 0.0), 2) for h in axis])


def main() -> int:
    if not DB.exists():
        print(f"数据库不存在：{DB}\n请先执行：sqlite3 {DB} < docs/db-schema.sql", file=sys.stderr)
        return 1

    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)   # 只读打开
    cur = con.cursor()

    today, month, total = cur.execute(SQL_REVENUE).fetchone()

    end_d = date.today()
    in_use, idle, fault = cur.execute(SQL_STATUS).fetchone()
    curve_hours, curve_kw = load_curve(cur, LOAD_CURVE_HOURS)

    fc_rows = cur.execute(SQL_FORECAST).fetchall()
    forecast = {
        "modelVersion": fc_rows[0][8] if fc_rows else "",
        "createTime": fc_rows[0][9] if fc_rows else "",
        "list": [{"stationId": r[0], "name": r[1], "horizon": r[2], "predictTime": r[3],
                  "loadKw": r[4], "idlePile": r[5], "isPeak": r[6], "congestion": r[7]}
                 for r in fc_rows],
    }

    since = (end_d - timedelta(days=BEHAVIOR_DAYS - 1)).isoformat()
    by_hour = dict(cur.execute(SQL_BEHAVIOR_HOUR, (since,)).fetchall())
    by_type = dict(cur.execute(SQL_BEHAVIOR_TYPE, (since,)).fetchall())
    by_status = dict(cur.execute(SQL_BEHAVIOR_STATUS, (since,)).fetchall())
    # 最后一条有观测的订单时刻：告诉大屏「数据截止到哪」。
    # 生成器刻意不写今天的订单（红线自检靠「今日无订单」分辨真实联调数据），
    # 所以纯演示数据下「今日营收」必然是 0，界面上必须标出数据截止时间，
    # 否则会被误读成「系统没在跑」。
    data_as_of = cur.execute(
        "SELECT MAX(settle_time) FROM t_order WHERE status = 3").fetchone()[0] or ""

    snap = {
        "generatedAt": datetime.now().strftime(FMT),
        "pollIntervalSec": POLL_INTERVAL_SEC,
        "dataAsOf": data_as_of,
        # 金额单位为「分」，前端除以 100 显示（CLAUDE.md 硬性规则 3）
        "revenue": {"today": today, "month": month, "total": total},
        "trend":   trend(cur, 7, end_d),          # 保留原字段名，前端旧逻辑不破
        "trend30": trend(cur, 30, end_d),         # 服务端 2302 支持 7 / 30 两档
        "status": [{"name": "在用", "value": in_use},
                   {"name": "闲置", "value": idle},
                   {"name": "故障", "value": fault}],
        "rank":   [{"name": n, "amount": a} for n, a in cur.execute(SQL_RANK)],
        "loadCurve": {"hours": curve_hours, "kw": curve_kw},
        "forecast": forecast,
        "behavior": {
            "hourlyOrders": [by_hour.get(h, 0) for h in range(24)],
            "byPileType": [{"name": "快充", "value": by_type.get(0, 0)},
                           {"name": "慢充", "value": by_type.get(1, 0)}],
            "orderStatus": [{"name": "已结算", "value": by_status.get(3, 0)},
                            {"name": "已取消", "value": by_status.get(4, 0)}],
            "windowDays": BEHAVIOR_DAYS,
        },
    }
    con.close()

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(snap, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"已导出 {OUT}")
    print(f"  电桩 {sum(x['value'] for x in snap['status'])} 台　站点 {len(snap['rank'])} 个　"
          f"预测 {len(forecast['list'])} 条（{forecast['modelVersion'] or '无'}）")
    print(f"  负荷曲线 {LOAD_CURVE_HOURS} 小时　用户行为窗口 {BEHAVIOR_DAYS} 天　"
          f"轮询间隔 {POLL_INTERVAL_SEC}s")
    print(f"  数据截止 {snap['dataAsOf'] or '无已结算订单'}")
    if not forecast["list"]:
        print("  ⚠ t_load_forecast 为空，预测层将不显示——先跑 ml/predict.py --commit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
