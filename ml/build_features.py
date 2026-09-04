#!/usr/bin/env python3
"""
ml/build_features.py  —  站-小时特征面板　归属 L5

从 t_order 构造训练用的站-小时面板，输出 ml/data/features.csv，供 train_forecast.py 使用。
[说明书] 1.4：预测未来 1h / 6h / 24h 各站点的充电负荷、空闲桩数量、高峰时段。

⚠ 运行期脚本，业务表一律 mode=ro 只读连接（ml/CLAUDE.md 数据库权限）。

## 为什么按 horizon 分层，而不是一张宽表

预测 t+h 时，**t+h 时刻的日历是已知的**（几点、周几、是否节假日不需要预测），
但**负荷观测只到 t**。两类特征的可见性边界不同，一张以 t 为行的宽表表达不了：
计划里写的「t−1h 滞后」只在 h=1 时成立，h=6 时 t−1h 根本还没发生，喂进去就是穿越。

所以每行 = (站点, horizon, 目标时刻)，并显式记两个时间戳：
- `origin_ts` = target_ts − horizon，特征可见的最后时刻，**所有滞后不得越过它**
- `target_ts` = 被预测的时刻，日历与天气特征取它

滞后统一用「相对 target 且 ≥ horizon」的口径：
- `lag_h`（= origin 时刻的值）—— 最近一次可见观测，是计划里 `t−1h` 的正确推广
- `lag_24` / `lag_168` —— 昨日同时 / 上周同时，24 和 168 都 ≥ 24 ≥ h，三个 horizon 都合法
- `roll_24` / `roll_168` —— 截止 origin 的滚动均值

h=24 时 `lag_24` 与 `lag_h` 取值相同（同一个时刻），列冗余但不构成穿越，保留以免
三个 horizon 的列名对不齐。

## 不放进特征的东西

**「历史同小时均值」不在这里算**。它是阶段 3 的对比基线，必须只用训练段拟合；
在全量数据上算好再喂进特征，等于把测试段的均值透给模型，评估会虚高。

**天气用类别码而非 `weather_factor`**。后者就是生成器里的需求乘数本身。
天气本身是可预报的外生变量，用它不算穿越；但要留意合成数据上这个特征
会比真实场景更干净，评估报告里得说明这一点。
"""
import argparse
import math
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime, timedelta
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_history import WEATHERS, resolve_profile   # noqa: E402  画像归类保持单一来源

DB_DEFAULT = Path("ml/data/dev.db")
DAY_FEATURES = Path("ml/data/day_features.csv")
OUT_DEFAULT = Path("ml/data/features.csv")
FMT = "%Y-%m-%d %H:%M:%S"
HORIZONS = (1, 6, 24)
MAX_LAG = 168


def parse_args():
    ap = argparse.ArgumentParser(description="构造站-小时特征面板")
    ap.add_argument("db", nargs="?", default=str(DB_DEFAULT))
    ap.add_argument("--out", default=str(OUT_DEFAULT))
    ap.add_argument("--horizons", default=",".join(map(str, HORIZONS)),
                    help="逗号分隔的预测步长，单位小时（默认 1,6,24）")
    return ap.parse_args()


def load_stations(con):
    stations = {}
    for sid, name in con.execute("SELECT station_id, name FROM t_station"):
        stations[sid] = {"station_name": name, "profile": resolve_profile(name)}
    for sid, ptype, cnt in con.execute(
            "SELECT station_id, type, COUNT(*) FROM t_pile GROUP BY station_id, type"):
        if sid in stations:
            stations[sid]["pile_fast" if ptype == 0 else "pile_slow"] = cnt
    for s in stations.values():
        s.setdefault("pile_fast", 0)
        s.setdefault("pile_slow", 0)
        s["pile_total"] = s["pile_fast"] + s["pile_slow"]
    return stations


def build_panel(con, stations):
    """站-小时面板：load_kw 与 active_sessions。

    load_kw：每个会话的电量按重叠时长摊到各小时桶，桶内能量即该小时的平均功率。
    active_sessions：**并发会话数**，按 Σ重叠秒数 / 3600 计，而不是「本小时内出现过
    几单」。后者会超过电桩数——一根快充桩一小时内可以跑完两单 20 分钟的会话——
    那样算出来的 idle_pile = 桩总数 − 并发数 会是负的，只能靠 clamp 兜底。
    按占用时长折算则天然 ≤ 桩总数，空闲桩数才是个有物理意义的量。
    """
    rows = con.execute(
        "SELECT station_id, start_time, end_time, kwh_x100 FROM t_order "
        "WHERE status = 3 AND start_time IS NOT NULL AND end_time IS NOT NULL"
    ).fetchall()
    if not rows:
        print("t_order 里没有已结算订单，先跑 gen_history.py 播种", file=sys.stderr)
        sys.exit(1)

    load = defaultdict(float)
    sess = defaultdict(float)
    t_min = t_max = None
    for sid, st, et, kwh_x100 in rows:
        s, e = datetime.strptime(st, FMT), datetime.strptime(et, FMT)
        total = (e - s).total_seconds()
        if total <= 0:
            continue
        t_min = s if t_min is None or s < t_min else t_min
        t_max = e if t_max is None or e > t_max else t_max
        cur = s.replace(minute=0, second=0, microsecond=0)
        while cur < e:
            nxt = cur + timedelta(hours=1)
            ov = (min(e, nxt) - max(s, cur)).total_seconds()
            if ov > 0:
                load[(sid, cur)] += kwh_x100 / 100.0 * ov / total
                sess[(sid, cur)] += ov / 3600.0
            cur = nxt

    # 补零成连续小时轴：缺的小时是「负荷为 0」，不是「没有观测」。
    # 漏补会让滞后特征错位——shift(24) 数的是行，不是小时。
    hours = pd.date_range(t_min.replace(minute=0, second=0, microsecond=0),
                          t_max.replace(minute=0, second=0, microsecond=0), freq="h")
    frames = []
    for sid in sorted(stations):
        frames.append(pd.DataFrame({
            "station_id": sid,
            "ts": hours,
            "load_kw": [load.get((sid, h.to_pydatetime()), 0.0) for h in hours],
            "active_sessions": [sess.get((sid, h.to_pydatetime()), 0.0) for h in hours],
        }))
    return pd.concat(frames, ignore_index=True)


def add_lags(panel, horizon):
    """按 horizon 生成滞后与滚动特征。全部相对 target 且回看 ≥ horizon 步。"""
    # shift 数的是行不是小时，面板必须按站-时严格有序，否则滞后会静默错位。
    out = panel.sort_values(["station_id", "ts"]).reset_index(drop=True)
    g = out.groupby("station_id", sort=False)
    for col, prefix in (("load_kw", "load"), ("active_sessions", "sess")):
        out[f"{prefix}_lag_h"] = g[col].shift(horizon)
        out[f"{prefix}_lag_24"] = g[col].shift(24)
        out[f"{prefix}_lag_168"] = g[col].shift(168)
        # 先滚动再 shift(horizon)：窗口右端落在 origin 上，不含 origin 之后的任何值。
        roll24 = g[col].transform(lambda s: s.rolling(24, min_periods=24).mean())
        roll168 = g[col].transform(lambda s: s.rolling(168, min_periods=168).mean())
        out[f"{prefix}_roll_24"] = roll24.groupby(out["station_id"], sort=False).shift(horizon)
        out[f"{prefix}_roll_168"] = roll168.groupby(out["station_id"], sort=False).shift(horizon)
    return out


def add_calendar(df, day_feat):
    """日历与天气特征，一律取 **target_ts** 的——这些在起报时刻就是已知的。"""
    ts = df["target_ts"]
    df["hour"] = ts.dt.hour
    df["weekday"] = ts.dt.weekday
    df["is_weekend"] = (df["weekday"] >= 5).astype(int)
    # 周期编码：23 时与 0 时相邻，整数编码表达不了这层环形关系，
    # 树模型会把它们切到最远的两个叶子上。
    df["hour_sin"] = (2 * math.pi * df["hour"] / 24).apply(math.sin)
    df["hour_cos"] = (2 * math.pi * df["hour"] / 24).apply(math.cos)
    df["dow_sin"] = (2 * math.pi * df["weekday"] / 7).apply(math.sin)
    df["dow_cos"] = (2 * math.pi * df["weekday"] / 7).apply(math.cos)

    codes = {name: i for i, (name, _, _) in enumerate(WEATHERS)}
    day_feat = day_feat.copy()
    day_feat["weather_code"] = day_feat["weather"].map(codes)
    df["date"] = ts.dt.date.astype(str)
    df = df.merge(day_feat[["date", "is_holiday", "weather", "weather_code"]],
                  on="date", how="left")
    return df


def verify_no_leak(feat, panel, horizon, n=200):
    """结构性校验：抽样核对 lag_h 确实等于 origin 时刻的观测值。

    滞后穿越是这类脚本最容易犯又最难发现的错——模型指标会好得离奇，
    但要到评估阶段才反应过来。这里直接拿原始面板对账，不靠 shift 的语义自觉。
    """
    key = panel.set_index(["station_id", "ts"])["load_kw"]
    sample = feat.dropna(subset=["load_lag_h"]).sample(
        min(n, len(feat.dropna(subset=["load_lag_h"]))), random_state=0)
    bad = 0
    for _, r in sample.iterrows():
        origin = r["target_ts"] - pd.Timedelta(hours=horizon)
        if r["origin_ts"] != origin:
            bad += 1
            continue
        if abs(key.get((r["station_id"], origin), float("nan")) - r["load_lag_h"]) > 1e-9:
            bad += 1
    if bad:
        print(f"滞后特征自检失败：{bad}/{len(sample)} 行的 lag_h 与 origin 观测对不上",
              file=sys.stderr)
        sys.exit(2)
    return len(sample)


def main() -> int:
    args = parse_args()
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"数据库不存在：{db_path}", file=sys.stderr)
        return 1
    if not DAY_FEATURES.exists():
        print(f"缺少 {DAY_FEATURES}，先跑 gen_history.py", file=sys.stderr)
        return 1
    horizons = [int(h) for h in args.horizons.split(",")]

    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    stations = load_stations(con)
    panel = build_panel(con, stations)
    con.close()

    day_feat = pd.read_csv(DAY_FEATURES)
    meta = pd.DataFrame([{"station_id": sid, **v} for sid, v in stations.items()])
    profiles = sorted(meta["profile"].unique())
    meta["profile_code"] = meta["profile"].map({p: i for i, p in enumerate(profiles)})

    parts, checked = [], 0
    for h in horizons:
        f = add_lags(panel, h)
        f = f.rename(columns={"ts": "target_ts",
                              "load_kw": "y_load_kw",
                              "active_sessions": "y_sessions"})
        f["horizon"] = h
        f["origin_ts"] = f["target_ts"] - pd.Timedelta(hours=h)
        f = add_calendar(f, day_feat)
        f = f.merge(meta, on="station_id", how="left")
        f["y_idle_pile"] = (f["pile_total"] - f["y_sessions"]).clip(lower=0)
        # 前 168+h 小时没有完整回看窗口，直接丢——补零会凭空造出「上周同时为 0」的假样本。
        f = f.dropna(subset=[c for c in f.columns if "_lag_" in c or "_roll_" in c])
        checked += verify_no_leak(f, panel, h)
        parts.append(f)

    feat = pd.concat(parts, ignore_index=True)
    cols = (["station_id", "station_name", "profile", "profile_code",
             "pile_total", "pile_fast", "pile_slow",
             "horizon", "origin_ts", "target_ts",
             "hour", "weekday", "is_weekend", "is_holiday",
             "hour_sin", "hour_cos", "dow_sin", "dow_cos", "weather", "weather_code"]
            + [c for c in feat.columns if "_lag_" in c or "_roll_" in c]
            + ["y_load_kw", "y_sessions", "y_idle_pile"])
    feat = feat[cols].sort_values(["horizon", "target_ts", "station_id"])

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    feat.to_csv(args.out, index=False)

    print(f"数据源：{db_path}")
    print(f"小时轴：{panel['ts'].min():%Y-%m-%d %H} ~ {panel['ts'].max():%Y-%m-%d %H}"
          f"（{panel['ts'].nunique()} 小时 × {panel['station_id'].nunique()} 站）")
    print(f"特征面板：{len(feat)} 行 × {len(cols)} 列，horizon {horizons}")
    for h in horizons:
        sub = feat[feat["horizon"] == h]
        print(f"  h={h:<3}{len(sub):6d} 行　目标区间 {sub['target_ts'].min():%m-%d %H} "
              f"~ {sub['target_ts'].max():%m-%d %H}")
    print(f"滞后自检：抽样 {checked} 行核对 lag_h 与 origin 观测，全部一致")
    zero_var = [c for c in feat.columns
                if feat[c].dtype.kind in "if" and feat[c].nunique() <= 1]
    if zero_var:
        print(f"⚠ 零方差列（建模时应丢弃）：{zero_var}")
    print(f"已写出 {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
