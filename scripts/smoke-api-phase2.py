#!/usr/bin/env python3
"""第二阶段 Flask API 冒烟测试　归属 L5。

用法：.venv-phase2/bin/python scripts/smoke-api-phase2.py [--base http://127.0.0.1:5000]

不只测正常路径——**异常路径几乎必漏**（DIVISION-OF-LABOR.md 对 L4 的预警），
所以这里对不存在的维度、不存在的接口、SQL 注入尝试都各测一条。
退出码 = 失败项数。
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

fails = 0


def get(base: str, path: str) -> tuple[int, dict]:
    url = base + path
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def check(name: str, cond: bool, detail: str = "") -> None:
    global fails
    if cond:
        print(f"  [PASS] {name}　{detail}")
    else:
        fails += 1
        print(f"  [FAIL] {name}　{detail}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:5000")
    args = ap.parse_args()
    base = args.base.rstrip("/")

    print("== 正常路径 ==\n")
    st, d = get(base, "/api/health")
    check("健康检查", st == 200 and d["code"] == 0 and d["data"]["database"],
          f"维度 {d['data']['dimensions']} 个，库连通 {d['data']['database']}")

    st, d = get(base, "/api/dimensions")
    n_dim = d.get("dimension_count", 0)
    n_cmp = d.get("comparison_count", 0)
    check("维度目录", st == 200 and d["code"] == 0, f"共 {d.get('total')} 份")
    # 这两条是老师的硬性指标，冒烟里直接断言，别等答辩才发现少了
    check("维度数 ≥ 8（[二阶段] 要求）", n_dim >= 8, f"实际 {n_dim} 个")
    check("对比分析 ≥ 2 组（[二阶段] 要求）", n_cmp >= 2, f"实际 {n_cmp} 组")

    st, d = get(base, "/api/overview")
    ov = d["data"]
    check("KPI 概览", st == 200 and d["code"] == 0,
          f"营收 {ov['revenue_fen']/100:.2f} 元　订单 {ov['order_total']}")
    # 金额必须是整数分：浮点金额是 CLAUDE.md 5.2 第 7 条明令禁止的
    check("金额为整数分（5.2 第 7 条）", isinstance(ov["revenue_fen"], int),
          f"revenue_fen 类型 {type(ov['revenue_fen']).__name__}")
    # Decimal 被序列化成字符串会让前端图表静默画不出来，这条专门防回归
    check("百分比为数字而非字符串", isinstance(ov["settle_rate_pct"], (int, float)),
          f"settle_rate_pct 类型 {type(ov['settle_rate_pct']).__name__}")

    st, d = get(base, "/api/dimension/c1_fast_vs_slow")
    rows = d["data"]
    check("取单个维度", st == 200 and len(rows) == 2, f"{len(rows)} 行")
    check("对比维度含快充与慢充",
          {r["type_label"] for r in rows} == {"快充", "慢充"},
          str(sorted(r["type_label"] for r in rows)))

    # 跨维度对账：所有含 revenue_fen 的维度必须加总一致，否则大屏各图会互相矛盾
    print("\n== 跨维度对账 ==\n")
    totals = {}
    for name in ("d1_revenue_trend", "d2_station_rank", "d4_hourly_load",
                 "c1_fast_vs_slow", "c2_weekday_vs_weekend"):
        _, dd = get(base, f"/api/dimension/{name}")
        totals[name] = sum(r["revenue_fen"] for r in dd["data"])
    uniq = set(totals.values())
    check("各维度营收合计一致", len(uniq) == 1,
          f"{uniq.pop() if len(uniq)==1 else totals} 分")

    print("\n== 异常路径 ==\n")
    st, d = get(base, "/api/dimension/not_exist")
    check("不存在的维度 → 404 + 错误码", st == 404 and d["code"] == 1002,
          f"code={d['code']}")

    # 表名会拼进 SQL，白名单是唯一防线，这条必须验
    inj = urllib.parse.quote("d1_revenue_trend; DROP TABLE d1_revenue_trend--")
    st, d = get(base, f"/api/dimension/{inj}")
    check("SQL 注入尝试被白名单挡下", st == 404 and d["code"] == 1002,
          f"code={d['code']}")
    st, d = get(base, "/api/dimensions")
    check("注入后维度目录完好", st == 200 and d.get("total", 0) > 0,
          f"仍有 {d.get('total')} 份")

    st, d = get(base, "/api/nope")
    check("不存在的接口 → 404", st == 404 and d["code"] == 1002, f"code={d['code']}")

    print(f"\n{'='*46}")
    print("RESULT:", "PASS" if fails == 0 else f"FAIL（{fails} 项）")
    return fails


if __name__ == "__main__":
    sys.exit(main())
