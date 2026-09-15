#!/usr/bin/env python3
"""
bigdata/spark/advice.py  —  智能运营建议　归属 L5

把 MLlib 的预测结果翻译成**运营动作**，产出两张结果集（同时写 JSON 与 MySQL）：

  d16_dispatch_advice  站点分流建议：预测拥堵的高峰站 → 推荐 2 个更空闲的替代站
  d17_alert_actions    预警处置清单：站点 × horizon 的预警、建议动作与**可信度**

数据来源全部是已有产物，不重训模型、不直连 charging.db（CLAUDE.md 5.2 第 10 条）：

  bigdata/analysis/d12_load_forecast.json   MLlib 预测（6 站 × 1/6/24h）
  bigdata/ods/t_station.csv                 站点经纬度（算距离）
  bigdata/ods/t_pile.csv                    站点故障桩数（status=2）
  bigdata/mllib/eval.json                   各 horizon 相对基线 B 的增益 → 可信度分档

口径：

  congestion = 1 − idle_pile / pile_total，与 predict.py 的 sessions/pile_total 同源；
  拥堵阈值 0.8（与管理端 2305、用户端 1101 一致），候选站要求 拥堵 < 0.5 且 空闲 ≥ 2；
  可信度：该 horizon 的两个预测目标（负荷 / 并发）里有几个跑赢基线 B —
          两个都赢 → 高，赢一个 → 中，都没赢 → 低。

幂等：每次运行 DROP + CREATE 两张表，重复跑结果一致。

用法：.venv-phase2/bin/python bigdata/spark/advice.py
"""
from __future__ import annotations

import configparser
import csv
import json
import math
import sys
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ANALYSIS = REPO / "bigdata" / "analysis"
ODS = REPO / "bigdata" / "ods"
EVAL = REPO / "bigdata" / "mllib" / "eval.json"
CFG = REPO / "config" / "phase2.ini"

BUSY = 0.8          # 拥堵阈值：≥ 触发分流建议
CAND_BUSY = 0.5     # 候选站拥堵上限
CAND_IDLE = 2       # 候选站最少预测空闲桩
CARE = 0.4          # 次一级阈值：拥堵度排前二且 ≥40% → 「引导」（预防性建议）


def load_rows(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return json.loads(path.read_text(encoding="utf-8")).get("rows", [])


def load_stations() -> dict[int, dict]:
    out: dict[int, dict] = {}
    with (ODS / "t_station.csv").open(encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            out[int(r["station_id"])] = {
                "name": r["name"], "lng": float(r["lng"]), "lat": float(r["lat"]),
            }
    return out


def load_faults() -> dict[int, int]:
    """各站故障桩数（status=2）。无故障桩的站不出现在字典里。"""
    out: dict[int, int] = {}
    with (ODS / "t_pile.csv").open(encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if int(r["status"]) == 2:
                sid = int(r["station_id"])
                out[sid] = out.get(sid, 0) + 1
    return out


def haversine(lng1: float, lat1: float, lng2: float, lat2: float) -> float:
    """球面距离，单位 km。"""
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lng2 - lng1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def load_confidence() -> dict[int, dict]:
    """各 horizon 的可信度分档：两个预测目标里跑赢基线的个数。

    只看单一目标的增益会在小样本上偏乐观或偏悲观（实测 MLlib 的负荷模型
    h=1 只有 +1.0%，而并发模型 +7.9%）；用「2/2 优于基线」这种计数口径，
    含义清楚、也不依赖人为设一个增益阈值。
    """
    if not EVAL.exists():
        return {}
    ev = json.loads(EVAL.read_text(encoding="utf-8"))
    gains: dict[int, dict[str, float]] = {}
    for r in ev.get("results", []):
        gains.setdefault(int(r["horizon"]), {})[str(r.get("target"))] = \
            float(r.get("gain_vs_b_pct", 0.0))

    label = {"y_load_kw": "负荷", "y_sessions": "并发"}
    out: dict[int, dict] = {}
    for h, g in gains.items():
        wins = sum(1 for v in g.values() if v >= 0)
        level = "高" if wins == len(g) else ("中" if wins >= 1 else "低")
        detail = "、".join(f"{label.get(k, k)} {'+' if v >= 0 else ''}{v:.1f}%"
                           for k, v in g.items())
        out[h] = {"level": level,
                  "reason": f"{h}h 预测 {detail}（{wins}/{len(g)} 优于基线）"}
    return out


def congestion_pct(row: dict) -> float:
    total = int(row.get("pile_total") or 0)
    if total <= 0:
        return 0.0
    return round((1 - int(row.get("idle_pile") or 0) / total) * 100, 1)


def build_dispatch(pred: list[dict], stations: dict[int, dict]) -> list[dict]:
    """A1：拥堵站 → 推荐更空闲的替代站（按 1 小时预测判断）。

    分两级，因为每站只有 4 桩——拥堵度只能按 25% 跳（空闲 2/4 = 50%），
    80% 阈值实际只在「空闲 0 桩」时才触发，模型基本不会给到那么满：
      分流（红）：拥堵度 ≥ 80%，与管理端 2305 预警同口径；
      引导（橙）：拥堵度排名前二且 ≥ 40%，属于预防性建议。
    """
    h1 = {int(r["station_id"]): r for r in pred if int(r["horizon"]) == 1}
    if not h1:
        return []

    ranked = sorted(h1.items(), key=lambda kv: -congestion_pct(kv[1]))
    picked = [(sid, r, "分流") for sid, r in ranked if congestion_pct(r) >= BUSY * 100]
    if not picked:
        picked = [(sid, r, "引导") for sid, r in ranked[:2] if congestion_pct(r) >= CARE * 100]
    if not picked:
        return []

    rows: list[dict] = []
    for sid, src, level in picked:
        cands = []
        for other, r in h1.items():
            if other == sid or congestion_pct(r) >= CAND_BUSY * 100:
                continue
            if int(r.get("idle_pile") or 0) < CAND_IDLE:
                continue
            a, b = stations.get(sid), stations.get(other)
            dist = (haversine(a["lng"], a["lat"], b["lng"], b["lat"])
                    if a and b else 999.0)
            cands.append((congestion_pct(r), dist, r))
        cands.sort(key=lambda x: (x[0], x[1]))

        row = {
            "advice_level": level,
            "from_station_id": sid,
            "from_station_name": src["station_name"],
            "from_congestion_pct": congestion_pct(src),
            "from_idle": int(src.get("idle_pile") or 0),
            "from_pile_total": int(src.get("pile_total") or 0),
        }
        texts = []
        for i, (cong, dist, r) in enumerate(cands[:2], start=1):
            row[f"to{i}_station_name"] = r["station_name"]
            row[f"to{i}_distance_km"] = round(dist, 1)
            row[f"to{i}_idle"] = int(r.get("idle_pile") or 0)
            row[f"to{i}_congestion_pct"] = cong
            texts.append(f"{r['station_name']}（{round(dist, 1)}km，预测空闲 "
                         f"{int(r.get('idle_pile') or 0)} 桩）")
        for i in (1, 2):
            row.setdefault(f"to{i}_station_name", "")
            row.setdefault(f"to{i}_distance_km", None)
            row.setdefault(f"to{i}_idle", None)
            row.setdefault(f"to{i}_congestion_pct", None)
        row["advice_text"] = (
            "引导用户分流至 " + "、".join(texts) if texts
            else "暂无更空闲的替代站，建议现场引导或增加临时桩"
        )
        rows.append(row)
    return rows


def build_actions(pred: list[dict], faults: dict[int, int],
                  conf: dict[int, dict]) -> list[dict]:
    """A3：站点 × horizon 的预警、建议动作与可信度。"""
    by_station: dict[int, list[dict]] = {}
    for r in pred:
        by_station.setdefault(int(r["station_id"]), []).append(r)

    rows: list[dict] = []
    for sid, items in by_station.items():
        lowest = min(items, key=lambda r: float(r.get("load_kw") or 0))
        for r in sorted(items, key=lambda x: int(x["horizon"])):
            h = int(r["horizon"])
            cong = congestion_pct(r)
            fault = faults.get(sid, 0)
            if cong >= BUSY * 100:
                kind = "拥堵"
                action = "引导分流至更空闲站点（见 A1 分流建议）"
            elif int(r.get("is_peak") or 0) == 1:
                kind = "高峰"
                action = (f"高峰前完成抢修 / 远程重启（该站 {fault} 台故障桩）"
                          if fault > 0 else "高峰时段加派值守")
            elif r is lowest:
                kind = "低谷"
                action = "可安排例行检修（低谷窗口）"
            else:
                continue
            c = conf.get(h, {"level": "—", "reason": "缺 eval.json，未能分档"})
            rows.append({
                "station_id": sid,
                "station_name": r["station_name"],
                "horizon": h,
                "predict_time": r.get("predict_time", ""),
                "alert_type": kind,
                "load_kw": float(r.get("load_kw") or 0),
                "idle_pile": int(r.get("idle_pile") or 0),
                "pile_total": int(r.get("pile_total") or 0),
                "congestion_pct": cong,
                "fault_piles": fault,
                "action": action,
                "confidence": c["level"],
                "confidence_reason": c["reason"],
            })
    rows.sort(key=lambda r: (r["confidence"] != "高", r["station_id"], r["horizon"]))
    return rows


DDL = {
    "d16_dispatch_advice": """
        CREATE TABLE `d16_dispatch_advice` (
          advice_level VARCHAR(4),
          from_station_id BIGINT, from_station_name VARCHAR(64),
          from_congestion_pct DOUBLE, from_idle BIGINT, from_pile_total BIGINT,
          to1_station_name VARCHAR(64), to1_distance_km DOUBLE,
          to1_idle BIGINT, to1_congestion_pct DOUBLE,
          to2_station_name VARCHAR(64), to2_distance_km DOUBLE,
          to2_idle BIGINT, to2_congestion_pct DOUBLE,
          advice_text VARCHAR(255)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        COMMENT='智能运营建议：高拥堵站 → 推荐更空闲替代站'""",
    "d17_alert_actions": """
        CREATE TABLE `d17_alert_actions` (
          station_id BIGINT, station_name VARCHAR(64), horizon BIGINT,
          predict_time VARCHAR(32), alert_type VARCHAR(8),
          load_kw DOUBLE, idle_pile BIGINT, pile_total BIGINT,
          congestion_pct DOUBLE, fault_piles BIGINT,
          action VARCHAR(255), confidence VARCHAR(4), confidence_reason VARCHAR(64)
        ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        COMMENT='智能运营建议：预警处置清单（含模型可信度）'""",
}


def write_mysql(registry: dict[str, list[dict]], meta: dict[str, str]) -> None:
    import pymysql
    cfg = configparser.ConfigParser()
    cfg.read(CFG, encoding="utf-8")
    m = cfg["mysql"]
    conn = pymysql.connect(host=m["host"], port=int(m["port"]), user=m["user"],
                           password=m["password"], database=m["database"],
                           charset="utf8mb4")
    try:
        with conn.cursor() as cur:
            for name, rows in registry.items():
                cur.execute(f"DROP TABLE IF EXISTS `{name}`")
                cur.execute(DDL[name])
                if not rows:
                    print(f"  {name:<28} 0 行 → MySQL（空表，页面显示空态）")
                    continue
                cols = list(rows[0].keys())
                ph = ",".join(["%s"] * len(cols))
                cur.executemany(
                    f"INSERT INTO `{name}` ({','.join(f'`{c}`' for c in cols)}) "
                    f"VALUES ({ph})",
                    [tuple(r.get(c) for c in cols) for r in rows])
                print(f"  {name:<28} {len(rows):>4} 行 → MySQL")
        conn.commit()
    finally:
        conn.close()


def update_summary(registry: dict[str, list[dict]], meta: dict[str, str]) -> None:
    """把两张建议表并入 _summary.json，保持文件与 MySQL 目录一致。"""
    path = ANALYSIS / "_summary.json"
    summary = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    dims = summary.setdefault("dimensions", {})
    dims.update(meta)
    summary["dimension_count"] = sum(1 for k in dims if k.startswith("d"))
    summary["comparison_count"] = len({k.split("_")[0] for k in dims if k.startswith("c")})
    summary["generated_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    pred = load_rows(ANALYSIS / "d12_load_forecast.json")
    if not pred:
        print("缺少 bigdata/analysis/d12_load_forecast.json —— 先跑 "
              ".venv-phase2/bin/python bigdata/mllib/predict.py", file=sys.stderr)
        return 1

    stations, faults, conf = load_stations(), load_faults(), load_confidence()
    registry = {
        "d16_dispatch_advice": build_dispatch(pred, stations),
        "d17_alert_actions": build_actions(pred, faults, conf),
    }
    meta = {
        "d16_dispatch_advice": "智能运营建议：高拥堵站 → 推荐更空闲替代站",
        "d17_alert_actions": "智能运营建议：预警处置清单（含模型可信度）",
    }

    print("== 智能运营建议 ==")
    ANALYSIS.mkdir(parents=True, exist_ok=True)
    for name, rows in registry.items():
        (ANALYSIS / f"{name}.json").write_text(
            json.dumps({"dimension": name, "description": meta[name],
                        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                        "row_count": len(rows), "rows": rows},
                       ensure_ascii=False, indent=2, default=str), encoding="utf-8")
        print(f"  {name:<28} {len(rows):>4} 行")

    update_summary(registry, meta)
    print("\n== 装载 MySQL ==")
    if CFG.exists():
        write_mysql(registry, meta)
    else:
        print(f"  [跳过] 缺少 {CFG.name}")
    print(f"\n结果集 {len(registry)} 份 → {ANALYSIS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
