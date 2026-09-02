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
from datetime import datetime
from pathlib import Path

DB = Path(sys.argv[1] if len(sys.argv) > 1 else "charging.db")
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else "dataviz/data/snapshot.json")

# 说明书 1.4 管理端/大屏指标，SQL 与 docs/db-schema.sql 末尾的参考查询一致
SQL_TREND = """
SELECT date(settle_time) AS d, IFNULL(SUM(amount),0) AS amt
  FROM t_order WHERE status = 3
   AND settle_time >= datetime('now','localtime','-7 day')
 GROUP BY d ORDER BY d"""
SQL_STATUS = "SELECT status, COUNT(*) FROM t_pile GROUP BY status"
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

STATUS_NAME = {0: "在用", 1: "闲置", 2: "故障"}   # [说明书] 1.4 三种状态


def main() -> int:
    if not DB.exists():
        print(f"数据库不存在：{DB}\n请先执行：sqlite3 {DB} < docs/db-schema.sql", file=sys.stderr)
        return 1

    con = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)   # 只读打开
    cur = con.cursor()

    today, month, total = cur.execute(SQL_REVENUE).fetchone()
    snap = {
        "generatedAt": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        # 金额单位为「分」，前端除以 100 显示（CLAUDE.md 硬性规则 3）
        "revenue": {"today": today, "month": month, "total": total},
        "trend":  [{"date": d, "amount": a} for d, a in cur.execute(SQL_TREND)],
        "status": [{"name": STATUS_NAME.get(s, str(s)), "value": c}
                   for s, c in cur.execute(SQL_STATUS)],
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
