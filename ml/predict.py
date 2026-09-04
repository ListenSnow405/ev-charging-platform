#!/usr/bin/env python3
"""
ml/predict.py  —  负荷预测回写 t_load_forecast　归属 L5

[说明书] 1.4：预测未来 1h / 6h / 24h 各站点的充电负荷、空闲桩数量、高峰时段。
用户端据此优先推荐低拥堵高空闲率站点（1101 的 congestion / idleForecast），
运营端负荷预警（2305）。

⚠ 权限边界：`t_load_forecast` 从一开始就是 L5 写、服务端与大屏只读，写它**不需要任何审批**。
   但其余业务表一律只读——这里不靠自觉，用 SQLite authorizer 把「只能写这一张表」
   变成连接级的结构性保证：任何对其他表的 INSERT/UPDATE/DELETE 与所有 DDL 都会被拒绝。

## 推理路径

`build_features.py` 只能为「已观测到 y」的时刻造行，造不出未来时刻的特征。
这里的做法：把面板向后延长 24 小时（y 填 NaN），再走同一套
`add_seasonal_mean` / `add_lags` / `add_calendar`。
延长段的 y 不会污染特征——所有滞后与滚动都 `shift(horizon)`，
取到的窗口右端落在 `origin` 上，从不触及 origin 之后的行。

`origin` = 面板上最后一个有观测的小时。三个目标时刻 = origin + 1 / 6 / 24。

## 派生规则

四个字段全部由**同一个预测量**派生，保证互相一致——单独给 is_peak 训分类器
会出现「负荷预测很低但标成高峰」的自相矛盾，演示时很难看。

- `idle_pile` = clamp(桩总数 − 预测并发数, 0, 桩总数)，**四舍五入取整**（列是 INTEGER）
- `congestion` = 1 − 空闲数 / 桩总数，用**取整前**的空闲数算。
  取整后再算的话，4 根桩只能得到 0/0.25/0.5/0.75/1 五个值，
  六个站排序会大量并列，1101 的 sortBy=1 推荐排序就失去了区分度
- `is_peak` = 预测负荷 ≥ `meta.json` 里该站该 horizon 的阈值。
  ⚠ 与计划的偏离：计划写的是「按站点当日预测曲线的分位数标记」，
  但只有 1/6/24 三个 horizon，三个点构不成曲线，当日分位数无从算起。
  阈值改由 `train_forecast.py` 在**验证段的样本外预测分布**上校准（P75），
  随模型一起交付——它是模型输出分布的函数，换模型必须跟着换。
  不能拿真实负荷的分位数当阈值：模型预测是平滑的，实测触发率只有 7.8%~12.8%，
  而校准目标是 25%，且 horizon 越大压得越低。对负荷预警来说这是错误方向

## 批次语义

协议 2305 规定：取同一 `model_version` 下 `create_time` 最大的一批。
因此**整批共用一个 `create_time`**，绝不逐行取当前时刻——逐行写会让同一批
出现多个 create_time，L2 的「取最新一批」查询会只捞到最后几行。

用法：
    .venv/bin/python ml/predict.py                     # dry-run，只打印不写库
    .venv/bin/python ml/predict.py --commit
    .venv/bin/python ml/predict.py charging.db --commit --prune
"""
import argparse
import json
import sqlite3
import sys
from datetime import datetime, timedelta
from pathlib import Path

import joblib
import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_features import (DAY_FEATURES, HORIZONS, add_calendar, add_lags,  # noqa: E402
                            add_seasonal_mean, build_panel, load_stations)
from gen_history import WEATHERS, is_statutory  # noqa: E402

DB_DEFAULT = Path("ml/data/dev.db")
MODEL_DIR = Path("ml/data/models")
FMT = "%Y-%m-%d %H:%M:%S"
WRITABLE = {"t_load_forecast", "sqlite_sequence"}   # 后者是 AUTOINCREMENT 的内部计数表


def parse_args():
    ap = argparse.ArgumentParser(description="预测未来 1h/6h/24h 负荷并回写 t_load_forecast")
    ap.add_argument("db", nargs="?", default=str(DB_DEFAULT))
    ap.add_argument("--commit", action="store_true", help="实际写库；缺省仅 dry-run 预览")
    ap.add_argument("--origin", default=None, metavar="'yyyy-MM-dd HH:00:00'",
                    help="指定起报时刻，默认取面板最后一个有观测的小时。"
                         "默认值下三个目标恰好落在 00/05/23 时的低谷，演示看不出东西，"
                         "也验不到 is_peak 分支——演示时用它把目标挪到高峰段")
    ap.add_argument("--check-skew", action="store_true",
                    help="拿 features.csv 对账推理特征，验证训练/推理无偏斜（需 --origin 落在历史区间）")
    ap.add_argument("--prune", action="store_true",
                    help="配合 --commit：写入后删除本次之前的所有预测批次")
    return ap.parse_args()


def only_forecast_writable(action, arg1, arg2, dbname, source):
    """SQLite authorizer：只放行对 t_load_forecast 的写，其余写操作与所有 DDL 一律拒绝。

    ml/CLAUDE.md 的「运行期脚本业务表只读」如果只靠代码自觉，一次手滑就破了。
    挂在连接上则是结构性的：即便脚本里写错了表名，SQLite 自己会拒绝。
    """
    if action in (sqlite3.SQLITE_INSERT, sqlite3.SQLITE_UPDATE, sqlite3.SQLITE_DELETE):
        return sqlite3.SQLITE_OK if arg1 in WRITABLE else sqlite3.SQLITE_DENY
    if action in (sqlite3.SQLITE_CREATE_TABLE, sqlite3.SQLITE_DROP_TABLE,
                  sqlite3.SQLITE_ALTER_TABLE, sqlite3.SQLITE_CREATE_INDEX,
                  sqlite3.SQLITE_DROP_INDEX):
        return sqlite3.SQLITE_DENY
    return sqlite3.SQLITE_OK


def truncate_to_origin(panel, origin_str):
    """把面板截断到指定 origin，模拟「站在那一刻」的可见范围。

    截断而不是仅仅改目标时刻：origin 之后的观测在起报时是看不到的，
    留在面板里会被滚动/扩展窗口捞进特征，那就是穿越。
    """
    origin = pd.Timestamp(origin_str)
    if origin > panel["ts"].max() or origin < panel["ts"].min():
        print(f"--origin 超出面板范围 {panel['ts'].min()} ~ {panel['ts'].max()}",
              file=sys.stderr)
        sys.exit(1)
    return panel[panel["ts"] <= origin].copy()


def extend_panel(panel, hours_ahead):
    """向后延长小时轴，y 填 NaN。

    填 NaN 而不是 0：0 是「负荷为零」的真实取值，会被 expanding/rolling 当成观测算进均值。
    NaN 则被 pandas 的滚动与扩展窗口跳过，语义才是「还没发生」。
    """
    origin = panel["ts"].max()
    future = pd.date_range(origin + pd.Timedelta(hours=1),
                           origin + pd.Timedelta(hours=hours_ahead), freq="h")
    pad = pd.DataFrame([{"station_id": sid, "ts": t, "load_kw": np.nan,
                         "active_sessions": np.nan}
                        for sid in panel["station_id"].unique() for t in future])
    return pd.concat([panel, pad], ignore_index=True), origin


def extend_day_features(day_feat, needed_dates):
    """未来日期的天气：沿用最后一个已知日。

    生成器的天气本就是持续性过程（马尔可夫，沿用概率 0.55），沿用是与之一致的最简假设。
    真实部署这里应接天气预报——报告里要写明这一点，不能让人以为模型真会预测天气。
    """
    known = set(day_feat["date"])
    last = day_feat.iloc[-1]
    rows = [day_feat]
    extra = [{"date": d, "weekday": datetime.strptime(d, "%Y-%m-%d").weekday(),
              "is_holiday": int(is_statutory(datetime.strptime(d, "%Y-%m-%d").date())),
              "weather": last["weather"], "weather_factor": last["weather_factor"]}
             for d in sorted(needed_dates) if d not in known]
    if extra:
        rows.append(pd.DataFrame(extra))
    return pd.concat(rows, ignore_index=True), [r["date"] for r in extra]


def check_skew(built, horizon, target_ts):
    """训练/推理偏斜自检：推理路径造的特征必须与 build_features.py 训练时造的逐列相等。

    这类偏斜不会报错，只会让模型在线上看到与训练时不同分布的输入，
    表现悄悄变差且极难归因。这里直接拿 features.csv 对账，不靠「两边代码看起来一样」。
    """
    from build_features import OUT_DEFAULT
    if not OUT_DEFAULT.exists():
        print(f"--check-skew 需要 {OUT_DEFAULT}，先跑 ml/build_features.py", file=sys.stderr)
        sys.exit(1)
    train = pd.read_csv(OUT_DEFAULT, parse_dates=["origin_ts", "target_ts"])
    ref = train[(train["horizon"] == horizon) & (train["target_ts"] == target_ts)]
    if ref.empty:
        print(f"h={horizon} 的目标时刻 {target_ts} 不在 features.csv 的历史区间内，"
              f"无法对账——把 --origin 往前挪", file=sys.stderr)
        sys.exit(1)
    cols = [c for c in train.columns
            if "_lag_" in c or "_roll_" in c or "_seas_" in c] + [
        "hour", "weekday", "is_weekend", "hour_sin", "hour_cos",
        "dow_sin", "dow_cos", "weather_code", "profile_code"]
    a = built.sort_values("station_id").reset_index(drop=True)
    b = ref.sort_values("station_id").reset_index(drop=True)
    bad = {c: float(np.max(np.abs(a[c].astype(float).to_numpy()
                                  - b[c].astype(float).to_numpy())))
           for c in cols}
    bad = {c: v for c, v in bad.items() if v > 1e-9}
    if bad:
        print(f"训练/推理特征偏斜（h={horizon}）：{bad}", file=sys.stderr)
        sys.exit(4)
    return len(cols)


def load_models(horizons):
    meta_path = MODEL_DIR / "meta.json"
    if not meta_path.exists():
        print(f"缺少 {meta_path}，先跑 ml/train_forecast.py", file=sys.stderr)
        sys.exit(1)
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    if not meta.get("peak_thresholds"):
        print("meta.json 缺少 peak_thresholds（is_peak 阈值），"
              "说明模型是旧版 train_forecast.py 训的，重跑一次训练", file=sys.stderr)
        sys.exit(2)
    models = {}
    for target in ("y_load_kw", "y_sessions"):
        for h in horizons:
            f = MODEL_DIR / f"{target}_h{h}.joblib"
            if not f.exists():
                print(f"缺少模型 {f}，先跑 ml/train_forecast.py", file=sys.stderr)
                sys.exit(1)
            b = joblib.load(f)
            if b["model_version"] != meta["model_version"]:
                print(f"{f.name} 的 model_version 与 meta.json 不一致"
                      f"（{b['model_version']} vs {meta['model_version']}），"
                      f"说明模型目录里混了两次训练的产物，重跑 train_forecast.py",
                      file=sys.stderr)
                sys.exit(2)
            models[(target, h)] = b
    thresholds = {int(h): {int(k): float(v) for k, v in d.items()}
                  for h, d in meta["peak_thresholds"].items()}
    return models, meta["model_version"], thresholds, meta.get("peak_quantile", 0.75)


def main() -> int:
    args = parse_args()
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"数据库不存在：{db_path}", file=sys.stderr)
        return 1
    if not DAY_FEATURES.exists():
        print(f"缺少 {DAY_FEATURES}，先跑 ml/gen_history.py", file=sys.stderr)
        return 1

    horizons = list(HORIZONS)
    models, model_version, peak_thr, peak_q = load_models(horizons)

    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    stations = load_stations(con)
    panel = build_panel(con, stations)
    con.close()

    if args.origin:
        panel = truncate_to_origin(panel, args.origin)
    panel, origin = extend_panel(panel, max(horizons))

    day_feat = pd.read_csv(DAY_FEATURES)
    targets = {h: origin + pd.Timedelta(hours=h) for h in horizons}
    day_feat, added = extend_day_features(
        day_feat, {t.strftime("%Y-%m-%d") for t in targets.values()})

    panel = add_seasonal_mean(panel)
    meta_df = pd.DataFrame([{"station_id": sid, **v} for sid, v in stations.items()])
    profiles = sorted(meta_df["profile"].unique())
    meta_df["profile_code"] = meta_df["profile"].map({p: i for i, p in enumerate(profiles)})

    create_time = datetime.now().strftime(FMT)   # 整批共用，见文件头「批次语义」
    rows = []
    for h in horizons:
        f = add_lags(panel, h)
        f = f.rename(columns={"ts": "target_ts", "load_kw": "y_load_kw",
                              "active_sessions": "y_sessions"})
        f["horizon"] = h
        f["origin_ts"] = f["target_ts"] - pd.Timedelta(hours=h)
        f = add_calendar(f, day_feat)
        f = f.merge(meta_df, on="station_id", how="left")
        f = f[f["target_ts"] == targets[h]]
        if len(f) != len(stations):
            print(f"h={h} 只造出 {len(f)}/{len(stations)} 个站的特征行，中止",
                  file=sys.stderr)
            return 3

        if args.check_skew:
            n = check_skew(f, h, targets[h])
            print(f"偏斜自检 h={h}：{n} 个特征列与 features.csv 逐列一致")

        pred = {}
        for target in ("y_load_kw", "y_sessions"):
            b = models[(target, h)]
            missing = [c for c in b["feat_cols"] if c not in f.columns]
            if missing:
                print(f"特征列缺失 {missing}，模型与特征脚本版本不匹配", file=sys.stderr)
                return 3
            # 严格按训练时的列序取，不能依赖 DataFrame 的自然列序
            pred[target] = np.clip(b["model"].predict(f[b["feat_cols"]]), 0, None)

        for i, (_, r) in enumerate(f.iterrows()):
            total = int(r["pile_total"])
            load_kw = float(pred["y_load_kw"][i])
            sessions = float(pred["y_sessions"][i])
            idle_raw = min(max(total - sessions, 0.0), float(total))
            rows.append({
                "station_id": int(r["station_id"]),
                "station_name": r["station_name"],
                "horizon": h,
                "predict_time": targets[h].strftime(FMT),
                "load_kw": round(load_kw, 2),
                "idle_pile": int(round(idle_raw)),
                # 用取整前的 idle_raw 算，保住排序区分度，见文件头「派生规则」
                "congestion": round(min(max(1.0 - idle_raw / total, 0.0), 1.0), 4),
                "is_peak": int(load_kw >= peak_thr[h][int(r["station_id"])]),
                "model_version": model_version,
                "create_time": create_time,
            })

    out = pd.DataFrame(rows)
    print(f"数据源：{db_path}　模型版本：{model_version}")
    print(f"起报时刻 origin = {origin:%Y-%m-%d %H:%M}（面板最后一个有观测的小时）")
    print(f"目标时刻：" + "　".join(f"h={h} → {t:%m-%d %H:%M}" for h, t in targets.items()))
    if added:
        print(f"⚠ 未来日期 {added} 不在 day_features.csv 中，天气沿用最后一个已知日"
              f"（{day_feat.iloc[-1]['weather']}）——真实部署此处应接天气预报")
    print(f"is_peak 阈值：随模型交付，取验证段预测分布 P{int(peak_q * 100)}（单位 kW）")
    for h in horizons:
        print(f"  h={h:<3}" + "　".join(f"{stations[s]['station_name'][:4]} {v:.1f}"
                                        for s, v in sorted(peak_thr[h].items())))
    print()
    for h in horizons:
        print(f"--- horizon = {h}h　目标 {targets[h]:%Y-%m-%d %H:%M} ---")
        sub = out[out["horizon"] == h]
        print(f"{'站点':<16}{'负荷kW':>9}{'空闲桩':>8}{'拥堵度':>9}{'高峰':>6}")
        for _, r in sub.iterrows():
            print(f"{r['station_name']:<16}{r['load_kw']:>9.1f}{r['idle_pile']:>8d}"
                  f"{r['congestion']:>9.3f}{'是' if r['is_peak'] else '否':>6}")
        print()

    if not args.commit:
        print("dry-run：未写入数据库。加 --commit 才会落库。")
        return 0

    con = sqlite3.connect(str(db_path))
    con.set_authorizer(only_forecast_writable)   # 结构性保证：除本表外一律不可写
    try:
        cols = ["station_id", "horizon", "predict_time", "load_kw", "idle_pile",
                "is_peak", "congestion", "model_version", "create_time"]
        con.executemany(
            f"INSERT INTO t_load_forecast ({','.join(cols)}) "
            f"VALUES ({','.join('?' * len(cols))})",
            [tuple(r[c] for c in cols) for _, r in out.iterrows()])
        pruned = 0
        if args.prune:
            pruned = con.execute("DELETE FROM t_load_forecast WHERE create_time < ?",
                                 (create_time,)).rowcount
        con.commit()
    finally:
        con.close()
    print(f"已写入 {len(out)} 行到 {db_path} 的 t_load_forecast"
          f"（{len(stations)} 站 × {len(horizons)} 个 horizon，create_time={create_time}）")
    if args.prune:
        print(f"并清除了本批次之前的 {pruned} 行历史预测")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
