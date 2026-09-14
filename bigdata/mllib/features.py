"""T7.1：Spark 特征工程。DWD → 站-小时面板 → 按 horizon 的监督学习样本。

口径继承第一阶段 `ml/build_features.py`（那套是踩过坑校准出来的，不重造）：

  · **load_kw**：每笔会话的电量按**重叠时长**摊到各小时桶。桶是 1 小时，
    所以桶内能量(kWh)在数值上就是该小时平均功率(kW)。
    不能用「本小时出现过就整笔计入」——跨 4 小时的慢充会被重复计 4 次。

  · **active_sessions**：并发会话数 = Σ重叠秒数 / 3600，**不是**「本小时出现过的会话数」。
    后者会让 idle_pile = 桩总数 − 并发数 变成负数，只能靠 clamp 兜底，是错的。

  · **季节均值特征**用扩展窗口，只聚合**严格早于当前样本**的同键观测，构造上无穿越。

与第一阶段的唯一口径差异：**不含天气特征**。天气是外生合成数据，不在业务库里，
引入它会让本流水线依赖 ODS 之外的产物（违反 CLAUDE.md 5.2 第 10 条）。
第一阶段评估报告本身也写明「合成数据上这个特征比现实更干净」，去掉反而更诚实。
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "spark"))
from spark_session import build_spark, REPO_ROOT          # noqa: E402

from pyspark.sql import functions as F                     # noqa: E402
from pyspark.sql import Window                             # noqa: E402

DWD = REPO_ROOT / "bigdata" / "dwd"
FEAT = REPO_ROOT / "bigdata" / "mllib" / "data"
HORIZONS = [1, 6, 24]
TARGETS = ["y_load_kw", "y_sessions"]


def build_panel(spark):
    """站-小时面板。返回 (station_id, ts, load_kw, active_sessions)。"""
    o = spark.read.parquet(str(DWD / "dwd_order.parquet"))
    settled = o.filter((F.col("status") == 3)
                       & F.col("start_time").isNotNull()
                       & F.col("end_time").isNotNull()
                       & (F.col("kwh_x100") > 0))

    # 把每笔会话炸成它覆盖到的每个整点桶
    sess = (settled
            .withColumn("s", F.col("start_time").cast("long"))
            .withColumn("e", F.col("end_time").cast("long"))
            .filter(F.col("e") > F.col("s"))
            .withColumn("kwh", F.col("kwh_x100") / 100.0)
            .withColumn("h0", (F.col("s") / 3600).cast("long") * 3600)
            .withColumn("h1", (F.col("e") / 3600).cast("long") * 3600)
            .withColumn("bucket", F.explode(F.sequence(F.col("h0"), F.col("h1"), F.lit(3600)))))

    apportioned = (sess
                   # 会话与该桶的重叠秒数
                   .withColumn("ov",
                               F.least(F.col("e"), F.col("bucket") + F.lit(3600))
                               - F.greatest(F.col("s"), F.col("bucket")))
                   .filter(F.col("ov") > 0)
                   .withColumn("dur", F.col("e") - F.col("s"))
                   # 电量按重叠比例摊；桶长 1h，故 kWh 数值 = 平均 kW
                   .withColumn("kwh_part", F.col("kwh") * F.col("ov") / F.col("dur"))
                   .withColumn("sess_part", F.col("ov") / 3600.0))

    agg = (apportioned.groupBy("station_id", "bucket")
           .agg(F.sum("kwh_part").alias("load_kw"),
                F.sum("sess_part").alias("active_sessions")))

    # 补零：没有订单的小时是**真实的 0 负荷**，不是缺失。漏掉它们会让模型只见过忙时。
    rng = agg.agg(F.min("bucket").alias("lo"), F.max("bucket").alias("hi")).collect()[0]
    hours = (spark.range(0, (rng["hi"] - rng["lo"]) // 3600 + 1)
             .withColumn("bucket", F.lit(rng["lo"]) + F.col("id") * 3600)
             .select("bucket"))
    stations = spark.read.parquet(str(DWD / "dwd_station.parquet")).select("station_id")
    grid = stations.crossJoin(hours)

    return (grid.join(agg, ["station_id", "bucket"], "left")
                .fillna({"load_kw": 0.0, "active_sessions": 0.0})
                .withColumn("ts", F.to_timestamp(F.from_unixtime("bucket")))
                .drop("bucket"))


def add_features(panel):
    """日历、季节均值、滞后、滚动。全部只用当前时刻及更早的信息。"""
    p = (panel
         .withColumn("hour", F.hour("ts"))
         .withColumn("weekday", F.dayofweek("ts") - 1)       # 0=周日
         .withColumn("is_weekend", F.dayofweek("ts").isin([1, 7]).cast("int"))
         .withColumn("hour_sin", F.sin(F.col("hour") * 2 * F.lit(3.141592653589793) / 24))
         .withColumn("hour_cos", F.cos(F.col("hour") * 2 * F.lit(3.141592653589793) / 24))
         .withColumn("dow_sin", F.sin(F.col("weekday") * 2 * F.lit(3.141592653589793) / 7))
         .withColumn("dow_cos", F.cos(F.col("weekday") * 2 * F.lit(3.141592653589793) / 7)))

    w_ts = Window.partitionBy("station_id").orderBy("ts")

    for col, pre in (("load_kw", "load"), ("active_sessions", "sess")):
        # 季节均值：同（站 × 小时 × 是否周末）的历史均值。
        # rowsBetween(...,-1) 是关键——**排除当前行**，否则就是拿答案当特征。
        w_seas = (Window.partitionBy("station_id", "hour", "is_weekend")
                  .orderBy("ts").rowsBetween(Window.unboundedPreceding, -1))
        p = p.withColumn(f"{pre}_seas_mean", F.avg(col).over(w_seas))

        p = (p.withColumn(f"{pre}_lag_1", F.lag(col, 1).over(w_ts))
              .withColumn(f"{pre}_lag_24", F.lag(col, 24).over(w_ts))
              .withColumn(f"{pre}_lag_168", F.lag(col, 168).over(w_ts))
              .withColumn(f"{pre}_roll_24",
                          F.avg(col).over(w_ts.rowsBetween(-24, -1)))
              .withColumn(f"{pre}_roll_168",
                          F.avg(col).over(w_ts.rowsBetween(-168, -1))))

    return p


def add_station_attrs(p, spark):
    """站点静态属性：装机规模与快慢充配比。

    这些是**静态**特征，但不同站装机差一倍，快慢充配比也不同——
    只给 station_id 的话，树要靠多次分裂才能间接学到容量，等于浪费深度。
    第一阶段的特征集里有 pile_total / pile_fast / pile_slow，这里补齐。
    """
    pile = spark.read.parquet(str(DWD / "dwd_pile.parquet"))
    attrs = (pile.groupBy("station_id")
             .agg(F.count("*").alias("pile_total"),
                  F.sum(F.when(F.col("type") == 0, 1).otherwise(0)).alias("pile_fast"),
                  F.sum(F.when(F.col("type") == 1, 1).otherwise(0)).alias("pile_slow"),
                  F.sum("power").alias("power_total")))
    return p.join(attrs, "station_id", "left")


def make_supervised(p, horizon: int):
    """把面板转成「用 t 时刻的特征预测 t+h 时刻的值」。

    直接多步（direct multi-step），不做递归预测——递归会让误差逐步累积，
    第一阶段已就此定案。
    """
    w = Window.partitionBy("station_id").orderBy("ts")
    out = (p.withColumn("y_load_kw", F.lead("load_kw", horizon).over(w))
            .withColumn("y_sessions", F.lead("active_sessions", horizon).over(w))
            .withColumn("target_ts", F.expr(f"ts + INTERVAL {horizon} HOURS"))
            .withColumn("horizon", F.lit(horizon)))
    # 滞后与滚动在序列头部必然为空（warm-up），这些行没法用
    need = ["load_lag_168", "sess_lag_168", "load_seas_mean", "sess_seas_mean",
            "y_load_kw", "y_sessions"]
    for c in need:
        out = out.filter(F.col(c).isNotNull())
    return out


FEATURE_COLS = [
    "station_id", "pile_total", "pile_fast", "pile_slow", "power_total",
    "hour", "weekday", "is_weekend",
    "hour_sin", "hour_cos", "dow_sin", "dow_cos",
    "load_seas_mean", "sess_seas_mean",
    "load_lag_1", "load_lag_24", "load_lag_168", "load_roll_24", "load_roll_168",
    "sess_lag_1", "sess_lag_24", "sess_lag_168", "sess_roll_24", "sess_roll_168",
]


def main() -> int:
    spark = build_spark("ecp-mllib-features")
    FEAT.mkdir(parents=True, exist_ok=True)

    panel = build_panel(spark).cache()
    n = panel.count()
    rng = panel.agg(F.min("ts"), F.max("ts")).collect()[0]
    print(f"站-小时面板：{n} 行　{rng[0]} ~ {rng[1]}")

    # 面板守恒自检：摊分后的总电量必须等于订单原始总电量，差一点都说明摊分写错了
    o = spark.read.parquet(str(DWD / "dwd_order.parquet"))
    src = (o.filter((F.col("status") == 3) & F.col("start_time").isNotNull()
                    & F.col("end_time").isNotNull() & (F.col("kwh_x100") > 0))
            .agg(F.sum(F.col("kwh_x100") / 100.0)).collect()[0][0])
    got = panel.agg(F.sum("load_kw")).collect()[0][0]
    drift = abs(src - got)
    print(f"电量守恒自检：订单 {src:.2f} 度　面板 {got:.2f} 度　偏差 {drift:.4f}")
    assert drift < 1.0, f"摊分丢失电量 {drift:.4f} 度——检查重叠时长计算"

    feat = add_station_attrs(add_features(panel), spark).cache()
    for h in HORIZONS:
        d = make_supervised(feat, h)
        d.write.mode("overwrite").parquet(str(FEAT / f"h{h}.parquet"))
        print(f"  horizon={h:>2}h　{d.count():>6} 行 × {len(FEATURE_COLS)} 特征")

    print(f"\n产出：{FEAT}")
    spark.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
