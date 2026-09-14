"""T7.4：用训练好的模型预测未来 1h/6h/24h，结果写 MySQL 供大屏取用。

预测三项（`[说明书]` 1.4 明文要求）：**负荷、空闲桩数、高峰时段**。
其中只有负荷与并发数是回归模型直出，另两项是**派生**的：

  · idle_pile = 桩总数 − 预测并发数（clip 到 0）
  · is_peak   = 该站当日预测曲线的分位数标记

沿用第一阶段的处理，不单独训分类器——单独训会出现「负荷预测很低但标成高峰」
这种自相矛盾的输出，那是把同一件事拆成两个模型各说各话。

起报点默认取面板最后一个小时。**演示时不要手动指定更早的起报点**：
predictTime 会落在过去，虚线会插进实线中间（第一阶段踩过，见 L5-PLAN 第 3 节）。
"""
from __future__ import annotations

import configparser
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "spark"))
from spark_session import build_spark, REPO_ROOT          # noqa: E402

from pyspark.ml import PipelineModel                       # noqa: E402
from pyspark.sql import functions as F                     # noqa: E402
from pyspark.sql import Window                             # noqa: E402

from features import build_panel, add_features, add_station_attrs   # noqa: E402

FEAT = REPO_ROOT / "bigdata" / "mllib" / "data"
MODELS = REPO_ROOT / "bigdata" / "mllib" / "models"
DWD = REPO_ROOT / "bigdata" / "dwd"
OUT = REPO_ROOT / "bigdata" / "analysis"
CFG = REPO_ROOT / "config" / "phase2.ini"
EVAL = REPO_ROOT / "bigdata" / "mllib" / "eval.json"

HORIZONS = [1, 6, 24]
PEAK_Q = 0.75          # 分位数阈值：预测负荷进入当站前 25% 即标为高峰


def main() -> int:
    spark = build_spark("ecp-mllib-predict")
    version = json.loads(EVAL.read_text(encoding="utf-8"))["model_version"]

    piles = (spark.read.parquet(str(DWD / "dwd_pile.parquet"))
             .groupBy("station_id").agg(F.count("*").alias("pile_total")))
    stations = (spark.read.parquet(str(DWD / "dwd_station.parquet"))
                .select("station_id", F.col("name").alias("station_name")))

    # 起报样本必须来自**特征面板**而非监督学习样本。
    #
    # h{N}.parquet 是训练用的，构造时过滤掉了标签为空的行（`lead(y, N)` 在序列尾部为空）。
    # 从它取「最新一行」，拿到的是**目标恰好落在数据末尾**的那一行——
    # 于是三个 horizon 的 predict_time 会全部等于同一个已知时刻（实测均为 09-04 23:00），
    # 那不是预测，是在报一个已经有观测值的时间点。
    #
    # 正确起报点是面板的最后一个小时（此刻特征齐备、标签本就不该存在），
    # 目标时刻 = 起报点 + h，全部落在数据之外。
    panel = build_panel(spark)
    feat_all = add_station_attrs(add_features(panel), spark)
    w_last = Window.partitionBy("station_id").orderBy(F.desc("ts"))
    origin = (feat_all.withColumn("_rk", F.row_number().over(w_last))
                      .filter(F.col("_rk") == 1).drop("_rk")
                      # 滞后/滚动特征在此处必须齐备，缺了说明面板长度不够
                      .filter(F.col("load_lag_168").isNotNull()
                              & F.col("load_seas_mean").isNotNull())).cache()
    n_origin = origin.count()
    assert n_origin > 0, "起报样本为空——面板长度不足 168 小时？"
    o_ts = origin.agg(F.max("ts")).collect()[0][0]
    print(f"起报点 {o_ts}（{n_origin} 站）\n")

    rows = []
    for h in HORIZONS:
        latest = origin.withColumn("target_ts",
                                   F.col("ts") + F.expr(f"INTERVAL {h} HOURS"))

        load = (PipelineModel.load(str(MODELS / f"y_load_kw_h{h}"))
                .transform(latest)
                .select("station_id", "ts", "target_ts",
                        F.col("prediction").alias("load_kw")))
        sess = (PipelineModel.load(str(MODELS / f"y_sessions_h{h}"))
                .transform(latest)
                .select("station_id", F.col("prediction").alias("sessions")))

        joined = (load.join(sess, "station_id").join(piles, "station_id", "left")
                      .join(stations, "station_id", "left")
                  # 负荷不可能为负；模型外推出负值时截到 0 而不是原样输出
                  .withColumn("load_kw", F.greatest(F.col("load_kw"), F.lit(0.0)))
                  .withColumn("sessions", F.greatest(F.col("sessions"), F.lit(0.0)))
                  .withColumn("idle_pile",
                              F.greatest(F.round(F.col("pile_total") - F.col("sessions")),
                                         F.lit(0.0)).cast("int"))
                  .withColumn("horizon", F.lit(h)))
        rows.append(joined)

    allp = rows[0]
    for r in rows[1:]:
        allp = allp.unionByName(r)
    allp = allp.cache()

    # is_peak：按**每个站**自己的预测分布定阈值。
    # 用全网统一阈值会让小站永远不是高峰、大站永远是高峰，失去意义。
    thr = (allp.groupBy("station_id")
           .agg(F.expr(f"percentile_approx(load_kw, {PEAK_Q})").alias("thr")))
    final = (allp.join(thr, "station_id")
             .withColumn("is_peak", (F.col("load_kw") >= F.col("thr")).cast("int"))
             .withColumn("model_version", F.lit(version))
             .withColumn("predict_time", F.date_format("target_ts", "yyyy-MM-dd HH:mm:ss"))
             .withColumn("origin_time", F.date_format("ts", "yyyy-MM-dd HH:mm:ss"))
             .withColumn("load_kw", F.round("load_kw", 2))
             .withColumn("sessions", F.round("sessions", 2))
             .select("station_id", "station_name", "horizon", "origin_time", "predict_time",
                     "load_kw", "sessions", "idle_pile", "is_peak", "pile_total",
                     "model_version")
             .orderBy("horizon", "station_id"))

    data = [r.asDict() for r in final.collect()]
    print(f"预测 {len(data)} 行（{len(HORIZONS)} 个 horizon × {len(data)//len(HORIZONS)} 站）"
          f"　模型版本 {version}\n")
    for r in data:
        print(f"  h={r['horizon']:>2}h  {r['station_name'][:10]:<12} "
              f"负荷 {r['load_kw']:>7.2f} kW　空闲 {r['idle_pile']:>2}/{r['pile_total']} 桩　"
              f"{'高峰' if r['is_peak'] else '平峰'}　→ {r['predict_time']}")

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "d12_load_forecast.json").write_text(
        json.dumps({"dimension": "d12_load_forecast",
                    "description": "MLlib 负荷预测：1h/6h/24h 负荷、空闲桩数、高峰时段",
                    "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                    "row_count": len(data), "rows": data},
                   ensure_ascii=False, indent=2, default=str), encoding="utf-8")

    if CFG.exists():
        write_mysql(data)
    else:
        print(f"\n[跳过 MySQL] 缺少 {CFG.name}")
    spark.stop()
    return 0


def write_mysql(rows: list[dict]) -> None:
    import pymysql
    cfg = configparser.ConfigParser()
    cfg.read(CFG, encoding="utf-8")
    m = cfg["mysql"]
    conn = pymysql.connect(host=m["host"], port=int(m["port"]), user=m["user"],
                           password=m["password"], database=m["database"], charset="utf8mb4")
    try:
        with conn.cursor() as cur:
            cur.execute("DROP TABLE IF EXISTS `d12_load_forecast`")
            cur.execute("""
                CREATE TABLE `d12_load_forecast` (
                  station_id BIGINT, station_name VARCHAR(64), horizon BIGINT,
                  origin_time VARCHAR(32), predict_time VARCHAR(32),
                  load_kw DOUBLE, sessions DOUBLE, idle_pile BIGINT,
                  is_peak BIGINT, pile_total BIGINT, model_version VARCHAR(64)
                ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
                COMMENT='MLlib 负荷预测：1h/6h/24h 负荷、空闲桩数、高峰时段'""")
            cols = ["station_id", "station_name", "horizon", "origin_time", "predict_time",
                    "load_kw", "sessions", "idle_pile", "is_peak", "pile_total",
                    "model_version"]
            cur.executemany(
                f"INSERT INTO d12_load_forecast ({','.join(cols)}) "
                f"VALUES ({','.join(['%s'] * len(cols))})",
                [tuple(r[c] for c in cols) for r in rows])
        conn.commit()
        print(f"\n✓ 已写入 MySQL d12_load_forecast（{len(rows)} 行）")
    finally:
        conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
