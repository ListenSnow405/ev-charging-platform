#!/usr/bin/env python3
"""
ml/train_forecast.py  —  负荷/并发数时序建模与评估　归属 L5

[说明书] 1.4：基于时序机器学习算法，预测未来 1h / 6h / 24h 各站点的充电负荷、
空闲桩数量、高峰时段。本脚本训练前两者（空闲桩数与高峰时段由预测值派生，见阶段 4）。

模型：HistGradientBoostingRegressor，**全局单模型 + station 特征**，不分站建模——
6 站 × 约 1270 小时，拆开每站样本太少。
2 个目标（load_kw / active_sessions）× 3 个 horizon = 6 个模型，直接多步，不做递归，
避免误差累积，也好解释。

切分：**按时间切，绝不随机切分**。随机切会把未来的样本混进训练集，
同一天相邻小时高度相关，指标能虚高到毫无意义。

超参选择：**从训练段末尾再切一段验证集**（时间序，不随机），在小网格上按验证 MAE 选，
选完用完整训练段重训。固定迭代数是不行的——诊断实测 h=6 时 400 轮的训练 MAE 19.3 /
测试 30.0，纯在学噪声；而不同 horizon 的最优容量差别很大。

基线：两条。
  A「历史同小时均值」（站 × 小时）——计划选定的基线。
  B「历史同小时均值（分工作日/周末）」（站 × 小时 × is_weekend）——更强的对照。
两条都只在**训练段**上拟合。加 B 的原因：特征工程阶段量到各站画像的周末效应
方向相反（办公型跌、休闲型涨），A 抓不到这部分，赢 A 不能说明模型学到了周内结构。
**以 B 为准**。

用法：
    .venv/bin/python ml/train_forecast.py
    .venv/bin/python ml/train_forecast.py --test-days 12 --features ml/data/features.csv
"""
import argparse
import json
import re
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.inspection import permutation_importance
from sklearn.metrics import mean_absolute_error, root_mean_squared_error

FEATURES_DEFAULT = Path("ml/data/features.csv")
MODEL_DIR = Path("ml/data/models")
REPORT_OUT = Path("ml/reports/forecast_eval.md")

TARGETS = {"y_load_kw": "负荷 kW", "y_sessions": "并发会话数"}
# is_peak 的分位数。0.75 = 把各站负荷最高的四分之一时段标为高峰。
PEAK_QUANTILE = 0.75
# 零方差列（六站种子数据相同 / 窗口内无法定节假日），以及 id、时间戳、目标列，都不进模型。
DROP = {"station_name", "profile", "weather", "origin_ts", "target_ts", "horizon",
        "pile_total", "pile_fast", "pile_slow", "is_holiday",
        "y_load_kw", "y_sessions", "y_idle_pile"}
CATEGORICAL = ["station_id", "profile_code", "weather_code"]


def parse_args():
    ap = argparse.ArgumentParser(description="训练 1h/6h/24h 负荷与并发数预测模型")
    ap.add_argument("--features", default=str(FEATURES_DEFAULT))
    ap.add_argument("--test-days", type=int, default=12, help="末尾多少天作为测试段（默认 12）")
    ap.add_argument("--valid-days", type=int, default=8,
                    help="从训练段末尾再切多少天做超参验证（默认 8）")
    ap.add_argument("--seed", type=int, default=42)
    return ap.parse_args()


def time_split(df, test_days):
    """按 target_ts 切，末尾 test_days 天作测试段。"""
    cut = df["target_ts"].max().normalize() - pd.Timedelta(days=test_days - 1)
    return df[df["target_ts"] < cut], df[df["target_ts"] >= cut], cut


def hour_mean_baseline(train, test, target, keys):
    """历史同小时均值。只用训练段拟合；测试段里没见过的组合回落到训练段总均值。"""
    table = train.groupby(keys)[target].mean()
    idx = pd.MultiIndex.from_frame(test[keys]) if len(keys) > 1 else pd.Index(test[keys[0]])
    return table.reindex(idx).fillna(train[target].mean()).to_numpy()


# 超参网格。刻意小——样本量只有约 5900 行，网格再大就是在验证集上过拟合。
GRID = [
    {"max_iter": 400, "learning_rate": 0.06, "max_leaf_nodes": 31},
    {"max_iter": 150, "learning_rate": 0.06, "max_leaf_nodes": 31},
    {"max_iter": 300, "learning_rate": 0.03, "max_leaf_nodes": 15},
    {"max_iter": 80, "learning_rate": 0.05, "max_leaf_nodes": 15},
    {"max_iter": 40, "learning_rate": 0.05, "max_leaf_nodes": 7},
    # 网格底部要足够保守：h≥6 时可用信号很弱，最优解接近「只复现季节均值」，
    # 容量给多了就是在学噪声。少了这两个点，并发数 h=24 会输给基线。
    {"max_iter": 200, "learning_rate": 0.02, "max_leaf_nodes": 7},
    {"max_iter": 20, "learning_rate": 0.05, "max_leaf_nodes": 7},
]


def make_model(params, cat_mask, seed):
    return HistGradientBoostingRegressor(
        loss="absolute_error",      # 与 MAE 指标一致；负荷分布右偏，平方损失会被峰值样本主导
        min_samples_leaf=20, l2_regularization=1.0,
        early_stopping=False,       # 早停要切验证集，这里改成显式的时序验证段，见下
        categorical_features=cat_mask, random_state=seed, **params)


def select_params(train, feat_cols, target, cat_mask, valid_days, seed):
    """在训练段末尾切验证集选超参。绝不用测试段选——那是拿答案调参。

    顺带返回最优配置在验证段上的**样本外预测**，用于校准 is_peak 阈值（见 peak_thresholds）。
    """
    inner_tr, valid, _ = time_split(train, valid_days)
    best, best_mae, best_pred = None, float("inf"), None
    for params in GRID:
        m = make_model(params, cat_mask, seed).fit(inner_tr[feat_cols], inner_tr[target])
        pv = np.clip(m.predict(valid[feat_cols]), 0, None)
        v = mean_absolute_error(valid[target], pv)
        if v < best_mae:
            best, best_mae, best_pred = params, v, pv
    return best, best_mae, valid.assign(_pred=best_pred)


def evaluate(y_true, y_pred):
    return mean_absolute_error(y_true, y_pred), root_mean_squared_error(y_true, y_pred)


def main() -> int:
    args = parse_args()
    fp = Path(args.features)
    if not fp.exists():
        print(f"缺少 {fp}，先跑 ml/build_features.py", file=sys.stderr)
        return 1

    df = pd.read_csv(fp, parse_dates=["origin_ts", "target_ts"])
    feat_cols = [c for c in df.columns if c not in DROP]
    cat_mask = [c in CATEGORICAL for c in feat_cols]
    model_version = "hgb-" + datetime.now().strftime("%Y%m%d-%H%M%S")
    MODEL_DIR.mkdir(parents=True, exist_ok=True)

    results, importances, per_station, peak_thresholds = [], {}, {}, {}
    for target, label in TARGETS.items():
        for h in sorted(df["horizon"].unique()):
            sub = df[df["horizon"] == h]
            train, test, cut = time_split(sub, args.test_days)

            params, valid_mae, valid_pred = select_params(train, feat_cols, target, cat_mask,
                                                          args.valid_days, args.seed)
            model = make_model(params, cat_mask, args.seed)
            model.fit(train[feat_cols], train[target])
            pred = model.predict(test[feat_cols])
            # 负荷与并发数都不可能为负，截断到 0——模型不知道这个物理约束
            pred = np.clip(pred, 0, None)

            base_a = hour_mean_baseline(train, test, target, ["station_id", "hour"])
            base_b = hour_mean_baseline(train, test, target, ["station_id", "hour", "is_weekend"])

            mae, rmse = evaluate(test[target], pred)
            mae_a, rmse_a = evaluate(test[target], base_a)
            mae_b, rmse_b = evaluate(test[target], base_b)
            results.append({
                "target": target, "label": label, "horizon": h,
                "n_train": len(train), "n_test": len(test), "cut": cut,
                "params": params, "valid_mae": valid_mae,
                "train_mae": mean_absolute_error(
                    train[target], np.clip(model.predict(train[feat_cols]), 0, None)),
                "mae": mae, "rmse": rmse,
                "mae_a": mae_a, "mae_b": mae_b, "rmse_b": rmse_b,
                "gain_a": (mae_a - mae) / mae_a * 100 if mae_a else float("nan"),
                "gain_b": (mae_b - mae) / mae_b * 100 if mae_b else float("nan"),
            })

            import joblib
            joblib.dump({"model": model, "feat_cols": feat_cols,
                         "target": target, "horizon": h, "model_version": model_version},
                        MODEL_DIR / f"{target}_h{h}.joblib")

            if target == "y_load_kw":
                # is_peak 的阈值：取模型**自身预测分布**的分位数，逐站逐 horizon 各一个。
                # 不能拿真实负荷的分位数当阈值——模型预测是平滑的（回归向均值），
                # 用真值分位数去卡平滑后的预测，触发率实测只有 7.8%~12.8%，
                # 而校准目标是 25%，且 horizon 越大压得越低（预测越平滑）。
                # 对负荷预警来说这是错误方向：该报的不报。
                # 在**验证段**上算而不是训练段：验证段是样本外的，预测的平滑程度
                # 才和上线后一致；训练段的拟合值更"尖"，会把阈值抬高，又回到欠触发。
                peak_thresholds[h] = (valid_pred.groupby("station_id")["_pred"]
                                      .quantile(PEAK_QUANTILE).round(3).to_dict())
                st = test[["station_name"]].copy()
                st["err_model"] = np.abs(test[target].to_numpy() - pred)
                st["err_base"] = np.abs(test[target].to_numpy() - base_b)
                per_station[h] = st.groupby("station_name").mean().assign(
                    gain=lambda d: (d.err_base - d.err_model) / d.err_base * 100)
                pi = permutation_importance(model, test[feat_cols], test[target],
                                            n_repeats=5, random_state=args.seed,
                                            scoring="neg_mean_absolute_error")
                importances[h] = pd.Series(pi.importances_mean, index=feat_cols).sort_values(
                    ascending=False)

    res = pd.DataFrame(results)
    (MODEL_DIR / "meta.json").write_text(json.dumps({
        "model_version": model_version,
        "trained_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "features": str(fp), "test_days": args.test_days,
        # is_peak 阈值随模型一起交付：它是模型输出分布的函数，换模型必须跟着换
        "peak_quantile": PEAK_QUANTILE,
        "peak_thresholds": {str(h): {str(k): v for k, v in d.items()}
                            for h, d in peak_thresholds.items()},
        "metrics": res.drop(columns=["cut"]).to_dict(orient="records"),
    }, ensure_ascii=False, indent=2), encoding="utf-8")

    # ---- 控制台 ----
    print(f"特征：{fp}　模型版本：{model_version}")
    cut = res["cut"].iloc[0]
    print(f"时序切分：训练段 < {cut:%Y-%m-%d}，测试段 ≥ {cut:%Y-%m-%d}"
          f"（{args.test_days} 天，{res['n_train'].iloc[0]} / {res['n_test'].iloc[0]} 行）\n")
    for target, label in TARGETS.items():
        print(f"【{label}】（MAE 越小越好，提升 = 相对基线的 MAE 降幅）")
        print(f"{'horizon':>8}{'训练 MAE':>11}{'模型 MAE':>11}{'基线A':>10}{'基线B':>10}"
              f"{'vs A':>9}{'vs B':>9}   选中超参")
        for _, r in res[res["target"] == target].iterrows():
            p = r["params"]
            print(f"{r['horizon']:>6}h{r['train_mae']:>11.3f}{r['mae']:>11.3f}"
                  f"{r['mae_a']:>10.3f}{r['mae_b']:>10.3f}"
                  f"{r['gain_a']:>8.1f}%{r['gain_b']:>8.1f}%   "
                  f"iter={p['max_iter']} lr={p['learning_rate']} leaf={p['max_leaf_nodes']}")
        print()

    print("【is_peak 阈值（各站 × horizon，取验证段预测分布的 P"
          f"{int(PEAK_QUANTILE * 100)}，单位 kW）】")
    print("  " + pd.DataFrame(peak_thresholds).round(1).to_string().replace("\n", "\n  "))
    print()
    print("【负荷模型 permutation importance（测试段，MAE 口径，前 8）】")
    for h, imp in importances.items():
        top = "　".join(f"{k} {v:.2f}" for k, v in imp.head(8).items())
        print(f"  h={h:<3}{top}")

    write_report(res, importances, per_station, cut, args, model_version, fp)
    print(f"\n模型已存 {MODEL_DIR}/，评估报告已写出 {REPORT_OUT}")
    return 0


def write_report(res, importances, per_station, cut, args, model_version, fp):
    L = []
    L.append("# 负荷预测精度评估报告\n")
    L.append("> 归属 L5，由 `ml/train_forecast.py` 自动生成。"
             f"模型版本 `{model_version}`，生成于 {datetime.now():%Y-%m-%d %H:%M}。\n")
    L.append("## 1. 任务与数据\n")
    L.append("[说明书] 1.4 要求预测未来 **1h / 6h / 24h** 各站点的充电负荷、空闲桩数量、"
             "高峰时段。本报告覆盖前两项的回归模型；空闲桩数与高峰时段由预测值派生"
             "（`idle_pile = 桩总数 − 预测并发数`，`is_peak` 按站点当日预测曲线分位数标记），"
             "不单独建模——单独训分类器会出现「负荷预测很低但标成高峰」的自相矛盾。\n")
    L.append(f"- 特征面板：`{fp}`，6 个站点 × 小时粒度，按 horizon 分层\n")
    L.append(f"- 训练 / 测试：**按时间切**，测试段为末尾 {args.test_days} 天"
             f"（≥ {cut:%Y-%m-%d}），训练 {res['n_train'].iloc[0]} 行 / 测试 "
             f"{res['n_test'].iloc[0]} 行\n")
    L.append("- **绝不随机切分**：同一天相邻小时高度相关，随机切会把未来样本混进训练集，"
             "指标虚高到没有意义\n")
    L.append("\n## 2. 模型\n")
    L.append("`HistGradientBoostingRegressor`，`loss=absolute_error`（与 MAE 指标一致；"
             "负荷分布右偏，平方损失会被少数峰值样本主导）。\n")
    L.append("**全局单模型 + station 特征**，不分站建模——每站样本量太少。"
             "3 个 horizon 各训一个模型（直接多步），不用递归预测，避免误差累积。\n")
    L.append("\n**超参选择**：从训练段末尾再切 "
             f"{args.valid_days} 天作验证集（时间序，不随机切），"
             "在一个 7 点小网格上按验证 MAE 选，选完用完整训练段重训。"
             "**绝不用测试段选超参**——那是拿答案调参。各 horizon 选中的配置见结果表："
             "h 越大选中的容量越小，与「可用信号越来越弱」是一致的。\n")
    L.append("\n## 3. 基线\n")
    L.append("- **基线 A**「历史同小时均值」：按（站点 × 小时）在**训练段**上取均值\n")
    L.append("- **基线 B**「分工作日/周末的同小时均值」：按（站点 × 小时 × 是否周末）\n")
    L.append("\n加基线 B 的原因：各站画像的周末效应方向相反（办公型周末跌、休闲型周末涨），"
             "基线 A 抓不到这部分，只赢 A 不能说明模型学到了周内结构。**以 B 为准**。\n")
    L.append("\n未选「上周同时」作基线：实测该口径与当前值的相关系数只有 0.04，"
             "拿它当基线是刷分，赢了也不说明问题。\n")
    L.append("\n**必须说明**：模型的特征里包含 `load_seas_mean` / `sess_seas_mean`——"
             "扩展窗口口径的同小时均值（只用严格早于当前样本的同键观测，构造上无穿越）。"
             "也就是说**基线本身是模型的一个输入**，模型理论上不会显著差于基线。"
             "因此下表的「vs B」不应读作「模型比基线聪明多少」，而应读作"
             "**「在季节均值之上还能再榨出多少」**。这样安排是有意的："
             "诊断阶段实测不给这个特征时，h=6 的模型测试 MAE 反而输给基线"
             "（30.05 vs 29.77），因为滞后特征在该 horizon 基本是噪声，模型在学噪声。\n")
    L.append("\n## 4. 结果\n")
    for target, label in TARGETS.items():
        L.append(f"\n### {label}（`{target}`）\n\n")
        L.append("| horizon | 训练 MAE | 测试 MAE | 基线 A | 基线 B | vs A | vs B | RMSE | 选中超参 |\n")
        L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
        for _, r in res[res["target"] == target].iterrows():
            q = r["params"]
            L.append(f"| {r['horizon']}h | {r['train_mae']:.3f} | {r['mae']:.3f} | "
                     f"{r['mae_a']:.3f} | {r['mae_b']:.3f} | {r['gain_a']:+.1f}% | "
                     f"**{r['gain_b']:+.1f}%** | {r['rmse']:.3f} | "
                     f"iter={q['max_iter']} lr={q['learning_rate']} leaf={q['max_leaf_nodes']} |\n")
    L.append("\n### 分站结果（负荷，相对基线 B 的 MAE 降幅）\n\n")
    L.append("| 站点 | " + " | ".join(f"h={h}" for h in sorted(per_station)) + " |\n")
    L.append("| --- | " + " | ".join("---" for _ in per_station) + " |\n")
    for name in sorted(next(iter(per_station.values())).index):
        cells = " | ".join(f"{per_station[h].loc[name, 'gain']:+.1f}%" for h in sorted(per_station))
        L.append(f"| {name} | {cells} |\n")
    L.append("\n## 5. 特征重要性（负荷模型，测试段 permutation importance，MAE 口径）\n\n")
    for h, imp in importances.items():
        L.append(f"\n**h={h}**：" + "，".join(f"`{k}` {v:.2f}" for k, v in imp.head(8).items()) + "\n")
    L.append("\n## 6. 讨论\n")
    L.append("\n**分站差异不小，且不完全按画像走。** 宝安中心（住宅型）三个 horizon 都在 "
             "+17% 以上，而南山科技园（办公型）在 h≥6 时是 −6.6%。同为办公型的福田CBD "
             "在 h=1 有 +10.4%。也就是说模型的增量主要来自少数几个站，"
             "对另一些站还不如直接用季节均值。站点数只有 6 个，"
             "这个差异有多少是真实规律、有多少是测试段 12 天的抽样波动，"
             "当前数据量下分不开——不宜过度解读。\n")
    L.append("\n**h=6 是结构性最难的一档。** 特征工程阶段实测：最近一次可见观测 `load_lag_h` "
             "与目标的相关系数，h=1 时 0.459、h=24 时 0.341，而 **h=6 只有 0.027**——"
             "6 小时差不多正好是日内周期的反相位（20 点的峰对 14 点的谷），"
             "最近观测反而没有信息。这一档模型只能靠 `lag_24` / `lag_168` 与日历特征撑着，"
             "MAE 明显差于 h=1 是数据结构决定的，不是模型缺陷。\n")
    L.append("\n**h≥6 时模型的最优策略就是复现季节均值。** 特征重要性表说明了这一点："
             "h=1 时首位是 `load_lag_h`（最近观测），而 h=6 / h=24 时首位变成 "
             "`load_seas_mean` 且断层领先，其余特征的贡献都在 0.5 以下。"
             "另一个佐证：诊断时只用日历特征（完全不给滞后）训练，h=24 的测试 MAE 是 29.745，"
             "与基线 B 的 29.747 几乎完全相同——24 小时之外，这份数据里日历以外的信息接近于零。\n")
    L.append("\n**并发数 h=24 与基线打平（−0.2%）是诚实结果，不是失败。** "
             "生成器的跨日持续性来自 AR(1)，去季节后的 t−24h 自相关只有 0.078，"
             "理论上可榨取的增量本就很小。继续调参把这个数字做成正的，"
             "只会是在测试集上过拟合。\n")
    L.append("\n**负荷与并发数是两个独立模型，低谷时段可能给出看似矛盾的组合。** "
             "例如某站预测负荷 0.0 kW、拥堵度却是 0.14（约 0.56 个并发会话）——"
             "历史数据里 `load_kw = 0` 与 `sessions = 0` 完全同现（各占 17.1%），"
             "这个组合在真实数据中从未出现。**但不应强行拉齐**："
             "实测这些行的真实负荷仅 1.8 kW（h=1），近零的负荷预测才是准确的一方，"
             "是并发数模型在低活跃时段小幅高估。试过按「每并发会话平均功率」"
             "给负荷加物理下限，结果这些行的 MAE 从 1.99 恶化到 16.10。"
             "对外只需说明：这类行的拥堵度本身就落在「很空闲」区间，不会误导推荐排序。\n")
    L.append("\n**合成数据的边界。** 训练数据由 `ml/gen_history.py` 生成，"
             "天气是可预报的外生变量，在真实场景里也会用天气预报，但合成数据上这个特征"
             "比现实更干净。报告中的绝对精度不宜直接外推到真实部署。\n")
    # 标题前必须留空行，否则紧跟在引用块/段落后的 "## " 会被当成上一块的续行
    md = re.sub(r"\n+(#{1,6} )", r"\n\n\1", "".join(L))
    REPORT_OUT.parent.mkdir(parents=True, exist_ok=True)
    REPORT_OUT.write_text(md, encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
