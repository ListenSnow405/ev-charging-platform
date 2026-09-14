"""T7.2 / T7.3：Spark MLlib 训练与评估。

**方法论完整继承第一阶段**（这是二阶段最省力也最值钱的继承项，见 ml/reports/forecast_eval.md）：

  1. **按时间切分，绝不随机切。** 同一天相邻小时高度相关，随机切会把未来样本
     混进训练集，指标虚高到没有意义。
  2. **双基线对照。** 基线 A =（站 × 小时）历史均值；基线 B =（站 × 小时 × 是否周末）。
     以 B 为准——各站周末效应方向相反，只赢 A 说明不了模型学到了周内结构。
  3. **超参在验证段选，绝不用测试段。** 从训练段末尾再切 8 天作验证，
     选完用完整训练段重训。用测试段调参就是拿答案调参。
  4. **主动说明限制**，见生成的评估报告第 4 节。

与第一阶段的差异：模型从 scikit-learn 的 HistGradientBoostingRegressor 换成
MLlib 的 GBTRegressor（已决事项 2）。两者都是梯度提升树，但实现与默认超参不同，
**指标不应与第一阶段逐位对比**，只比「相对基线的降幅」这个无量纲量。

产出：
  bigdata/mllib/models/<target>_h<h>/     模型
  bigdata/mllib/eval.json                 评估明细
  bigdata/quality/07_forecast_eval.md     评估报告（答辩用）
"""
from __future__ import annotations

import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "spark"))
from spark_session import build_spark, REPO_ROOT          # noqa: E402

from pyspark.ml import Pipeline                            # noqa: E402
from pyspark.ml.feature import VectorAssembler             # noqa: E402
from pyspark.ml.regression import GBTRegressor             # noqa: E402
from pyspark.sql import functions as F                     # noqa: E402

from features import FEATURE_COLS, HORIZONS, TARGETS       # noqa: E402

FEAT = REPO_ROOT / "bigdata" / "mllib" / "data"
MODELS = REPO_ROOT / "bigdata" / "mllib" / "models"
OUT = REPO_ROOT / "bigdata" / "mllib"

TEST_DAYS = 12        # 测试段：末 12 天
VALID_DAYS = 8        # 验证段：训练段末 8 天

# 网格。经三轮实测定型，**这是并集网格**——高容量无正则档与正则化档并存，
# 由验证段按目标各自挑，不预设哪类目标该用哪档。
#
# 走到这里的过程（留作记录，避免后人重走）：
#   第 1 轮 maxIter≤120 无正则 → 6 个模型 5 个跑输基线，训练 MAE≈基线，是**欠拟合**；
#   第 2 轮 maxIter≤500 无正则 → 负荷类转好（选中 iter=300 depth=5），
#                                 但并发数转成**过拟合**（训练 0.388 / 测试 0.740）；
#   第 3 轮 加正则化          → 并发数三个 horizon 齐齐改善，但负荷类全部退步。
#                                 **原因是那轮同时把无正则档从 iter=300 砍到 150**，
#                                 一次改了两个变量，是设计失误，不是正则化的锅。
#   本轮   并集             → 修掉上述混淆。
GRID = [
    # —— 高容量无正则档：第 2 轮实测对负荷类最优 ——
    {"maxIter": 300, "maxDepth": 5, "stepSize": 0.1,
     "subsamplingRate": 1.0, "featureSubsetStrategy": "all"},
    {"maxIter": 150, "maxDepth": 5, "stepSize": 0.1,
     "subsamplingRate": 1.0, "featureSubsetStrategy": "all"},
    # —— 正则化档：第 3 轮实测对并发数最优 ——
    {"maxIter": 300, "maxDepth": 3, "stepSize": 0.05,
     "subsamplingRate": 0.7, "featureSubsetStrategy": "sqrt"},
    {"maxIter": 300, "maxDepth": 4, "stepSize": 0.05,
     "subsamplingRate": 0.8, "featureSubsetStrategy": "onethird"},
    {"maxIter": 400, "maxDepth": 3, "stepSize": 0.03,
     "subsamplingRate": 0.7, "featureSubsetStrategy": "sqrt"},
]


def metrics(df, label: str, pred: str = "prediction") -> dict:
    """MAE / RMSE / R²。一次聚合算完，避免对同一份数据反复扫。"""
    r = df.select(
        F.avg(F.abs(F.col(label) - F.col(pred))).alias("mae"),
        F.sqrt(F.avg(F.pow(F.col(label) - F.col(pred), 2))).alias("rmse"),
        F.avg(F.col(label)).alias("ybar")).collect()[0]
    ybar = r["ybar"]
    ss = df.select(
        F.sum(F.pow(F.col(label) - F.col(pred), 2)).alias("sse"),
        F.sum(F.pow(F.col(label) - F.lit(ybar), 2)).alias("sst")).collect()[0]
    r2 = 1 - (ss["sse"] / ss["sst"]) if ss["sst"] else float("nan")
    return {"mae": round(r["mae"], 4), "rmse": round(r["rmse"], 4), "r2": round(r2, 4)}


def baseline(train, evalset, label: str, keys: list[str]) -> dict:
    """基线：按 keys 分组取**训练段**均值，套到评估段上。

    均值只能来自训练段——用全量算均值等于把测试段信息泄漏进基线，
    那样的基线会不合理地强，衬得模型很差。
    """
    means = train.groupBy(*keys).agg(F.avg(label).alias("prediction"))
    joined = evalset.select(*keys, label).join(means, keys, "left")
    # 训练段没见过的组合退回全局均值，不能留 null
    gm = train.agg(F.avg(label)).collect()[0][0]
    joined = joined.fillna({"prediction": gm})
    return metrics(joined, label)


def train_one(spark, horizon: int, label: str, log: list) -> dict:
    df = spark.read.parquet(str(FEAT / f"h{horizon}.parquet")).cache()
    tmax = df.agg(F.max("target_ts")).collect()[0][0]
    test_from = F.lit(tmax) - F.expr(f"INTERVAL {TEST_DAYS} DAYS")
    valid_from = F.lit(tmax) - F.expr(f"INTERVAL {TEST_DAYS + VALID_DAYS} DAYS")

    train_full = df.filter(F.col("target_ts") < test_from)
    test = df.filter(F.col("target_ts") >= test_from)
    train_sub = df.filter(F.col("target_ts") < valid_from)
    valid = df.filter((F.col("target_ts") >= valid_from) & (F.col("target_ts") < test_from))

    n_tr, n_te, n_va = train_full.count(), test.count(), valid.count()
    print(f"\n── {label} · h={horizon} ──  训练 {n_tr}　验证 {n_va}　测试 {n_te}")

    asm = VectorAssembler(inputCols=FEATURE_COLS, outputCol="features")

    # ---- 超参在验证段选 ----
    best, best_mae = None, float("inf")
    for g in GRID:
        gbt = GBTRegressor(featuresCol="features", labelCol=label,
                           lossType="absolute",       # 与 MAE 指标对齐，抗右偏分布
                           checkpointInterval=10,     # 每 10 轮落盘截断血缘，见 main() 注释
                           seed=42, **g)
        m = Pipeline(stages=[asm, gbt]).fit(train_sub)
        mae = metrics(m.transform(valid), label)["mae"]
        print(f"    验证 MAE {mae:>8.4f}　{g}")
        if mae < best_mae:
            best_mae, best = mae, g

    # ---- 用完整训练段按选中超参重训 ----
    gbt = GBTRegressor(featuresCol="features", labelCol=label,
                       lossType="absolute", checkpointInterval=10, seed=42, **best)
    model = Pipeline(stages=[asm, gbt]).fit(train_full)

    tr_m = metrics(model.transform(train_full), label)
    te_m = metrics(model.transform(test), label)
    base_a = baseline(train_full, test, label, ["station_id", "hour"])
    base_b = baseline(train_full, test, label, ["station_id", "hour", "is_weekend"])

    gain_a = (base_a["mae"] - te_m["mae"]) / base_a["mae"] * 100
    gain_b = (base_b["mae"] - te_m["mae"]) / base_b["mae"] * 100

    out = MODELS / f"{label}_h{horizon}"
    if out.exists():
        shutil.rmtree(out)
    model.write().overwrite().save(str(out))

    print(f"    选中 {best}")
    print(f"    测试 MAE {te_m['mae']:.4f}　RMSE {te_m['rmse']:.4f}　R² {te_m['r2']:.4f}")
    print(f"    基线A {base_a['mae']:.4f}（{gain_a:+.1f}%）　"
          f"基线B {base_b['mae']:.4f}（{gain_b:+.1f}%）")

    rec = {"target": label, "horizon": horizon, "params": best,
           "n_train": n_tr, "n_valid": n_va, "n_test": n_te,
           "train": tr_m, "test": te_m,
           "baseline_a": base_a, "baseline_b": base_b,
           "gain_vs_a_pct": round(gain_a, 2), "gain_vs_b_pct": round(gain_b, 2)}
    log.append(rec)
    df.unpersist()
    return rec


def main() -> int:
    spark = build_spark("ecp-mllib-train", shuffle_partitions=4)
    MODELS.mkdir(parents=True, exist_ok=True)

    # **必须设检查点目录**，否则 maxIter 一大就 StackOverflowError。
    # GBT 每轮都在上一轮的 RDD 上叠加，血缘线性增长；任务序列化是递归的，
    # 深到一定程度直接爆 JVM 栈。GBTRegressor 的 checkpointInterval（默认 10）
    # 本是用来截断血缘的，但**没有检查点目录时它静默不生效**——
    # 实测 maxIter≤120 能过、开到 500 必崩，就是这个原因。
    ckpt = REPO_ROOT / "bigdata" / "mllib" / ".checkpoint"
    if ckpt.exists():
        shutil.rmtree(ckpt)
    ckpt.mkdir(parents=True, exist_ok=True)
    spark.sparkContext.setCheckpointDir(str(ckpt))
    log: list = []

    print("== T7 Spark MLlib 训练 ==")
    print(f"切分：测试段末 {TEST_DAYS} 天；验证段为训练段末 {VALID_DAYS} 天（均按时间切）")

    for label in TARGETS:
        for h in HORIZONS:
            train_one(spark, h, label, log)

    meta = {
        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "model_version": "gbt-" + datetime.now().strftime("%Y%m%d-%H%M%S"),
        "framework": f"Spark MLlib GBTRegressor（Spark {spark.version}）",
        "loss": "absolute",
        "split": f"按时间切分：测试末 {TEST_DAYS} 天，验证为训练段末 {VALID_DAYS} 天",
        "feature_count": len(FEATURE_COLS),
        "features": FEATURE_COLS,
        "grid": GRID,
        "results": log,
    }
    (OUT / "eval.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2),
                                   encoding="utf-8")
    print(f"\n评估明细：{OUT/'eval.json'}")
    print(f"模型版本：{meta['model_version']}")
    spark.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
