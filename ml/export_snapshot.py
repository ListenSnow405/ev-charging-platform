#!/usr/bin/env python3
"""
ml/export_snapshot.py  —  大屏数据快照导出　归属 L5

技术基线（CLAUDE.md 第 2 节「大屏取数」）：
    L5 只读 SQLite，定时导出 JSON 快照，大屏轮询静态文件。
    服务端因此保持纯 Socket，L2 不必额外写 HTTP 服务。

⚠ 只读，禁止写业务表。预测结果写 t_load_forecast（独立预测表）。

用法：python3 ml/export_snapshot.py [charging.db] [dataviz/data/snapshot.json]
"""
import json
import sqlite3
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

DB = Path(sys.argv[1] if len(sys.argv) > 1 else "charging.db")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "dataviz/data/snapshot.json")

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
SQL_REVENUE = """
SELECT
  IFNULL(SUM(CASE WHEN date(settle_time)=date('now','localtime')      THEN amount END),0),
  IFNULL(SUM(CASE WHEN strftime('%Y-%m',settle_time)=strftime('%Y-%m','now','localtime') THEN amount END),0),
  IFNULL(SUM(amount),0)
FROM t_order WHERE status = 3"""

def main() -> int:
    if not DB.exists():
        print(f"数据库不存在：{DB}\n请先执行：sqlite3 {DB} < docs/db-schema.sql", file=sys.stderr)
        return 1

    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)   # 只读打开
    cur = con.cursor()

    today, month, total = cur.execute(SQL_REVENUE).fetchone()

    TREND_DAYS = 7
    end_d = date.today()
    start_d = end_d - timedelta(days=TREND_DAYS - 1)
    got = dict(cur.execute(SQL_TREND, (start_d.isoformat(), end_d.isoformat())).fetchall())
    trend = [{"date": (start_d + timedelta(days=i)).isoformat(),
              "amount": got.get((start_d + timedelta(days=i)).isoformat(), 0)}
             for i in range(TREND_DAYS)]

    in_use, idle, fault = cur.execute(SQL_STATUS).fetchone()

    snap = {
        "generatedAt": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        # 金额单位为「分」，前端除以 100 显示（CLAUDE.md 硬性规则 3）
        "revenue": {"today": today, "month": month, "total": total},
        "trend":  trend,
        "status": [{"name": "在用", "value": in_use},
                   {"name": "闲置", "value": idle},
                   {"name": "故障", "value": fault}],
        "rank":   [{"name": n, "amount": a} for n, a in cur.execute(SQL_RANK)],
    }
    con.close()

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(snap, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"已导出 {OUT}（电桩 {sum(x['value'] for x in snap['status'])} 台，"
          f"站点 {len(snap['rank'])} 个）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
